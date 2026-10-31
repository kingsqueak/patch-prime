// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// patchprime-pluginhost: runs one plugin as a PipeWire node for the engine,
// in a process of its own so a crashing plugin can't take the engine down
// (see PluginRunner for the protocol), or lists the plugins installed.
//
//   patchprime-pluginhost --scan      the plugins, as JSON (see PluginInfo)
//   patchprime-pluginhost --run ...   one plugin node (see PluginRunner)
//
// Exit codes: 0 when stdin closes (the engine let go), 2 when the plugin
// can't be loaded (with {"error": ...} on stdout first).

#include "busmanager.h"
#include "effects.h"
#include "lv2.h"
#include "pipewiregraph.h"
#include "plugins.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>

#include <array>
#include <cstdio>
#include <memory>
#include <unistd.h>

namespace {

void say(const QJsonObject &o)
{
    const auto line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}

int fail(const QString &message)
{
    say({{QStringLiteral("error"), message}});
    return 2;
}

QJsonArray list(const auto &values, std::size_t count)
{
    QJsonArray out;
    for (std::size_t i = 0; i < count; ++i) {
        out.append(static_cast<double>(values.at(i)));
    }
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("patchprime-pluginhost"));

    QCommandLineParser parser;
    const QCommandLineOption scan(QStringLiteral("scan"), QStringLiteral("List the installed plugins as JSON."));
    const QCommandLineOption run(QStringLiteral("run"), QStringLiteral("Run one plugin as a node."));
    const QCommandLineOption uri(QStringLiteral("uri"), QStringLiteral("The plugin."), QStringLiteral("uri"));
    const QCommandLineOption name(QStringLiteral("name"), QStringLiteral("The node name."), QStringLiteral("name"));
    const QCommandLineOption positions(QStringLiteral("positions"), QStringLiteral("Channel positions, e.g. FL,FR."), QStringLiteral("list"));
    const QCommandLineOption rate(QStringLiteral("rate"), QStringLiteral("Sample rate."), QStringLiteral("hz"), QStringLiteral("48000"));
    const QCommandLineOption owner(QStringLiteral("owner"), QStringLiteral("The engine's owner tag."), QStringLiteral("tag"));
    const QCommandLineOption description(QStringLiteral("description"), QStringLiteral("The node description."), QStringLiteral("text"));
    const QCommandLineOption params(QStringLiteral("params"), QStringLiteral("Parameters as JSON."), QStringLiteral("json"));
    const QCommandLineOption bypassed(QStringLiteral("bypassed"), QStringLiteral("Start bypassed."));
    parser.addOptions({scan, run, uri, name, positions, rate, owner, description, params, bypassed});
    parser.addHelpOption();
    parser.process(app);

    pp::Lv2World world;

    if (parser.isSet(scan)) {
        const auto json = pp::pluginsToJson(world.scan());
        std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (!parser.isSet(run) || !parser.isSet(uri) || !parser.isSet(name)) {
        parser.showHelp(1);
    }

    const auto *plugin = world.find(parser.value(uri).toStdString());
    if (plugin == nullptr) {
        return fail(QStringLiteral("the plugin isn't installed"));
    }
    if (const auto problem = pp::Lv2World::problem(plugin); !problem.empty()) {
        return fail(QString::fromStdString(problem));
    }
    std::vector<std::string> channels;
    for (const auto &p : parser.value(positions).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        channels.push_back(p.toStdString());
    }
    if (channels.empty() || channels.size() > static_cast<std::size_t>(pp::maxEffectChannels)) {
        return fail(QStringLiteral("no channels"));
    }

    auto effect = std::make_unique<pp::Lv2Effect>(world, plugin);
    pp::Lv2Effect &lv2 = *effect;
    const auto initial = QJsonDocument::fromJson(parser.value(params).toUtf8()).object();
    for (auto it = initial.begin(); it != initial.end(); ++it) {
        lv2.setParam(it.key().toStdString(), it.value().toDouble());
    }
    lv2.enabled = !parser.isSet(bypassed);
    std::vector<std::unique_ptr<pp::Effect>> effects;
    effects.push_back(std::move(effect));
    // Making the chain prepares the plugin: instantiates and activates it.
    auto chain = std::make_unique<pp::EffectChain>(std::move(effects), parser.value(rate).toInt(),
                                                   static_cast<int>(channels.size()));
    if (!lv2.error().empty()) {
        return fail(QString::fromStdString(lv2.error()));
    }
    auto processor = std::make_shared<pp::Processor>();
    processor->setChain(std::move(chain));

    // The node, made (again) whenever PipeWire is there.
    pp::PipeWireGraph graph;
    const auto nodeName = parser.value(name).toStdString();
    const pp::Properties props{
        {"media.class", std::string(pp::effectsMediaClass)},
        {"node.description", parser.value(description).toStdString()},
        {"patchprime.fx.owner", parser.value(owner).toStdString()},
        {"node.group", "patchprime.fx"},
    };
    QObject::connect(&graph, &pp::PipeWireGraph::connectedChanged, &app, [&](bool connected) {
        if (connected && graph.addFilter(nodeName, channels, props, processor)) {
            say({{QStringLiteral("ready"), true}});
        }
    });
    graph.start();

    // Commands from the engine; the host ends when the engine lets go.
    QByteArray pending;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        std::array<char, 4096> buffer{};
        const auto n = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (n <= 0) {
            input.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        pending.append(buffer.data(), n);
        qsizetype end = 0;
        while ((end = pending.indexOf('\n')) >= 0) {
            const auto o = QJsonDocument::fromJson(pending.left(end)).object();
            pending.remove(0, end + 1);
            if (const auto set = o.value(QLatin1String("set")).toObject(); !set.isEmpty()) {
                const auto values = set.value(QLatin1String("params")).toObject();
                for (auto it = values.begin(); it != values.end(); ++it) {
                    lv2.setParam(it.key().toStdString(), it.value().toDouble());
                }
                if (const auto on = set.value(QLatin1String("enabled")); on.isBool()) {
                    lv2.enabled = on.toBool();
                }
            }
            if (const auto metering = o.value(QLatin1String("metering")); metering.isBool()) {
                processor->setMetering(metering.toBool());
            }
        }
    });

    // The plugin's input and output levels while the engine wants them.
    QTimer meters;
    meters.setInterval(50);
    QObject::connect(&meters, &QTimer::timeout, &app, [&] {
        auto *current = processor->chain();
        if (!processor->isMetering() || current == nullptr) {
            return;
        }
        const auto levels = current->takeLevels();
        if (levels.empty()) {
            return;
        }
        const auto &l = levels.front();
        say({{QStringLiteral("levels"),
              QJsonObject{{QStringLiteral("groups"), static_cast<int>(l.groups)},
                          {QStringLiteral("inPeak"), list(l.inPeak, l.groups)},
                          {QStringLiteral("inRms"), list(l.inRms, l.groups)},
                          {QStringLiteral("outPeak"), list(l.outPeak, l.groups)},
                          {QStringLiteral("outRms"), list(l.outRms, l.groups)}}}});
    });
    meters.start();

    const int code = QCoreApplication::exec();
    graph.stop();
    return code;
}

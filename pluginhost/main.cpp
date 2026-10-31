// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// patchprime-pluginhost: runs one plugin (LV2 or CLAP) as a PipeWire node
// for the engine, in a process of its own so a crashing plugin can't take
// the engine down (see PluginRunner for the protocol), or lists the plugins
// installed.
//
//   patchprime-pluginhost --scan      the plugins, as JSON (see PluginInfo);
//                                     each .clap file is read by
//   patchprime-pluginhost --scan-clap <file>   in a process of its own
//   patchprime-pluginhost --run ...   one plugin node (see PluginRunner),
//                                     and its window when asked
//
// Exit codes: 0 when stdin closes (the engine let go), 2 when the plugin
// can't be loaded (with {"error": ...} on stdout first).

#include "busmanager.h"
#include "clap.h"
#include "effects.h"
#include "lv2.h"
#include "pipewiregraph.h"
#include "plugins.h"
#include "x11window.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QProcess>
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
    const QCommandLineOption scanClap(QStringLiteral("scan-clap"), QStringLiteral("List the plugins in one .clap file as JSON."), QStringLiteral("file"));
    const QCommandLineOption format(QStringLiteral("format"), QStringLiteral("lv2 or clap."), QStringLiteral("format"), QStringLiteral("lv2"));
    const QCommandLineOption binary(QStringLiteral("binary"), QStringLiteral("CLAP: the .clap file."), QStringLiteral("file"));
    const QCommandLineOption run(QStringLiteral("run"), QStringLiteral("Run one plugin as a node."));
    const QCommandLineOption uri(QStringLiteral("uri"), QStringLiteral("The plugin."), QStringLiteral("uri"));
    const QCommandLineOption name(QStringLiteral("name"), QStringLiteral("The node name."), QStringLiteral("name"));
    const QCommandLineOption positions(QStringLiteral("positions"), QStringLiteral("Channel positions, e.g. FL,FR."), QStringLiteral("list"));
    const QCommandLineOption rate(QStringLiteral("rate"), QStringLiteral("Sample rate."), QStringLiteral("hz"), QStringLiteral("48000"));
    const QCommandLineOption owner(QStringLiteral("owner"), QStringLiteral("The engine's owner tag."), QStringLiteral("tag"));
    const QCommandLineOption description(QStringLiteral("description"), QStringLiteral("The node description."), QStringLiteral("text"));
    const QCommandLineOption params(QStringLiteral("params"), QStringLiteral("Parameters as JSON."), QStringLiteral("json"));
    const QCommandLineOption bypassed(QStringLiteral("bypassed"), QStringLiteral("Start bypassed."));
    const QCommandLineOption title(QStringLiteral("title"), QStringLiteral("The plugin window's title."), QStringLiteral("text"));
    const QCommandLineOption stateFollows(QStringLiteral("state-follows"), QStringLiteral("Read the plugin's state from the first line of stdin."));
    parser.addOptions({scan, scanClap, format, binary, run, uri, name, positions, rate, owner,
                       description, params, bypassed, title, stateFollows});
    parser.addHelpOption();
    parser.process(app);

    if (parser.isSet(scanClap)) {
        const auto json = pp::pluginsToJson(pp::scanClapFile(parser.value(scanClap).toStdString()));
        std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    pp::Lv2World world;

    if (parser.isSet(scan)) {
        // LV2 here (reading descriptions only); CLAP plugins have to be
        // loaded to be read, so each file in a process of its own.
        auto plugins = world.scan();
        for (const auto &file : pp::clapFiles()) {
            QProcess one;
            one.setProcessChannelMode(QProcess::ForwardedErrorChannel);
            one.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--scan-clap"), QString::fromStdString(file)});
            if (!one.waitForFinished(20000) || one.exitStatus() != QProcess::NormalExit ||
                one.exitCode() != 0) {
                one.kill();
                // NOLINTNEXTLINE(modernize-use-std-print): std::print may throw, main mustn't
                std::fprintf(stderr, "patchprime-pluginhost: %s couldn't be read\n", file.c_str());
                continue;
            }
            for (auto &p : pp::pluginsFromJson(one.readAllStandardOutput())) {
                plugins.push_back(std::move(p));
            }
        }
        const auto json = pp::pluginsToJson(plugins);
        std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (!parser.isSet(run) || !parser.isSet(uri) || !parser.isSet(name)) {
        parser.showHelp(1);
    }
    if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
        pp::initX11Threads(); // before any plugin (or its window) touches X
    }
    // The plugin's own state, saved before: {"state": "..."} on the first line.
    std::string initialState;
    if (parser.isSet(stateFollows)) {
        std::string line;
        char c = 0;
        while (::read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {
            line += c;
        }
        initialState = QJsonDocument::fromJson(QByteArray::fromStdString(line))
                           .object()
                           .value(QLatin1String("state"))
                           .toString()
                           .toStdString();
    }

    const bool clap = parser.value(format) == QLatin1String("clap");
    const LilvPlugin *lv2Plugin = nullptr;
    if (!clap) {
        lv2Plugin = world.find(parser.value(uri).toStdString());
        if (lv2Plugin == nullptr) {
            return fail(QStringLiteral("the plugin isn't installed"));
        }
        if (const auto problem = pp::Lv2World::problem(lv2Plugin); !problem.empty()) {
            return fail(QString::fromStdString(problem));
        }
    } else if (parser.value(binary).isEmpty()) {
        return fail(QStringLiteral("the plugin isn't installed"));
    }
    std::vector<std::string> channels;
    for (const auto &p : parser.value(positions).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        channels.push_back(p.toStdString());
    }
    if (channels.empty() || channels.size() > static_cast<std::size_t>(pp::maxEffectChannels)) {
        return fail(QStringLiteral("no channels"));
    }

    std::unique_ptr<pp::HostedPlugin> effect;
    if (clap) {
        effect = std::make_unique<pp::ClapEffect>(parser.value(binary).toStdString(),
                                                  parser.value(uri).toStdString());
        if (!effect->error().empty()) {
            return fail(QString::fromStdString(effect->error()));
        }
    } else {
        effect = std::make_unique<pp::Lv2Effect>(world, lv2Plugin);
    }
    pp::HostedPlugin &plugin = *effect;
    const auto initial = QJsonDocument::fromJson(parser.value(params).toUtf8()).object();
    for (auto it = initial.begin(); it != initial.end(); ++it) {
        plugin.setParam(it.key().toStdString(), it.value().toDouble());
    }
    plugin.enabled = !parser.isSet(bypassed);
    plugin.setInitialState(initialState);
    std::vector<std::unique_ptr<pp::Effect>> effects;
    effects.push_back(std::move(effect));
    // Making the chain prepares the plugin: instantiates and activates it.
    auto chain = std::make_unique<pp::EffectChain>(std::move(effects), parser.value(rate).toInt(),
                                                   static_cast<int>(channels.size()));
    if (!plugin.error().empty()) {
        return fail(QString::fromStdString(plugin.error()));
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

    // The plugin's own window, while it's open, and its state, sent to the
    // engine when it changes (checked while the window is open, when it
    // closes, and when asked).
    std::unique_ptr<pp::PluginWindow> window;
    std::string lastState = initialState;
    const auto sendState = [&] {
        if (auto state = plugin.saveState(); state != lastState) {
            lastState = std::move(state);
            say({{QStringLiteral("state"), QString::fromStdString(lastState)}});
        }
    };
    // Parameters changed in the plugin's window, for the engine to keep.
    const auto sayParam = [](const std::string &paramName, double value) {
        say({{QStringLiteral("param"), QJsonObject{{QStringLiteral("name"), QString::fromStdString(paramName)},
                                                   {QStringLiteral("value"), value}}}});
    };
    const auto closeWindow = [&] {
        if (!window) {
            return;
        }
        window.reset();
        say({{QStringLiteral("window"), false}});
        sendState();
    };
    const auto openWindow = [&] {
        if (window) {
            window->raise();
            return;
        }
        window = plugin.openWindow(parser.value(title).toStdString());
        if (!window->error().empty()) {
            say({{QStringLiteral("windowError"), QString::fromStdString(window->error())}});
            window.reset();
            say({{QStringLiteral("window"), false}});
            return;
        }
        window->onParam = sayParam;
        say({{QStringLiteral("window"), true}});
    };
    QTimer windowTimer;
    windowTimer.setInterval(33);
    QObject::connect(&windowTimer, &QTimer::timeout, &app, [&] {
        if (window && !window->idle()) {
            closeWindow();
        }
        plugin.takeParamChanges(sayParam); // ones the plugin reports itself (CLAP)
    });
    windowTimer.start();
    QTimer stateTimer;
    stateTimer.setInterval(5000);
    QObject::connect(&stateTimer, &QTimer::timeout, &app, [&] {
        if (plugin.takeStateDirty() || window) {
            sendState();
        }
    });
    stateTimer.start();

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
                    plugin.setParam(it.key().toStdString(), it.value().toDouble());
                }
                if (const auto on = set.value(QLatin1String("enabled")); on.isBool()) {
                    plugin.enabled = on.toBool();
                }
            }
            if (const auto metering = o.value(QLatin1String("metering")); metering.isBool()) {
                processor->setMetering(metering.toBool());
            }
            if (const auto show = o.value(QLatin1String("window")); show.isBool()) {
                if (show.toBool()) {
                    openWindow();
                } else {
                    closeWindow();
                }
            }
            if (o.value(QLatin1String("saveState")).toBool()) {
                sendState();
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
    window.reset();
    graph.stop();
    return code;
}

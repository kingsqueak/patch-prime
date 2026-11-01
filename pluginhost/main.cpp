// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// patchprime-pluginhost: runs one plugin (LV2, CLAP or VST3) as a PipeWire node
// for the engine, in a process of its own so a crashing plugin can't take
// the engine down (see PluginRunner for the protocol), or lists the plugins
// installed.
//
//   patchprime-pluginhost --scan      the plugins, as JSON (see PluginInfo);
//                                     each .clap file is read by
//   patchprime-pluginhost --scan-clap <file>   in a process of its own,
//   patchprime-pluginhost --scan-vst3 <bundle> and each VST3 bundle by
//   patchprime-pluginhost --run ...   one plugin node (see PluginRunner),
//                                     and its window when asked
//
// Exit codes: 0 when stdin closes (the engine let go), 2 when the plugin
// can't be loaded (with {"error": ...} on stdout first), 3 when the plugin
// asked to be started again (a VST3 reloading itself).

#include "busmanager.h"
#include "clap.h"
#include "effects.h"
#include "lv2.h"
#include "pipewiregraph.h"
#include "plugins.h"
#include "vst3.h"
#include "x11window.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <unistd.h>

namespace {

// The engine's end of stdout. Plugins print too (to stdout, as often as not):
// stdout itself is pointed at stderr, and the protocol has its own copy.
int g_protocol = STDOUT_FILENO; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once, at start

void claimStdout()
{
    if (const int copy = ::dup(STDOUT_FILENO); copy >= 0) {
        g_protocol = copy;
        ::dup2(STDERR_FILENO, STDOUT_FILENO);
    }
}

void writeAll(const QByteArray &bytes)
{
    const char *at = bytes.constData();
    auto left = static_cast<std::size_t>(bytes.size());
    while (left > 0) {
        const auto n = ::write(g_protocol, at, left);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return; // the engine is gone
        }
        at += n; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        left -= static_cast<std::size_t>(n);
    }
}

void say(const QJsonObject &o)
{
    writeAll(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
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

// The most a line from the engine may be (a plugin's state can be large,
// an impulse response say, but not without bound).
constexpr qsizetype maxLine = qsizetype{64} << 20;

// One line from stdin, read before the event loop runs; nullopt at the end
// of the input, or for a line too long.
std::optional<QByteArray> readLine()
{
    QByteArray line;
    std::array<char, 4096> buffer{};
    while (true) {
        // A byte at a time up to the line's end: what follows is the event
        // loop's to read.
        const auto n = ::read(STDIN_FILENO, buffer.data(), 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return std::nullopt;
        }
        if (buffer[0] == '\n') {
            return line;
        }
        line.append(buffer[0]);
        if (line.size() > maxLine) {
            return std::nullopt;
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    claimStdout();
    if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
        pp::initX11Threads(); // before any plugin (or its window) touches X
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("patchprime-pluginhost"));

    QCommandLineParser parser;
    const QCommandLineOption scan(QStringLiteral("scan"), QStringLiteral("List the installed plugins as JSON."));
    const QCommandLineOption scanClap(QStringLiteral("scan-clap"), QStringLiteral("List the plugins in one .clap file as JSON."), QStringLiteral("file"));
    const QCommandLineOption scanVst3(QStringLiteral("scan-vst3"), QStringLiteral("List the plugins in one .vst3 bundle as JSON."), QStringLiteral("bundle"));
    const QCommandLineOption format(QStringLiteral("format"), QStringLiteral("lv2, clap or vst3."), QStringLiteral("format"), QStringLiteral("lv2"));
    const QCommandLineOption binary(QStringLiteral("binary"), QStringLiteral("CLAP: the .clap file; VST3: the bundle."), QStringLiteral("file"));
    const QCommandLineOption run(QStringLiteral("run"), QStringLiteral("Run one plugin as a node; its parameters and state come first on stdin."));
    const QCommandLineOption uri(QStringLiteral("uri"), QStringLiteral("The plugin."), QStringLiteral("uri"));
    const QCommandLineOption name(QStringLiteral("name"), QStringLiteral("The node name."), QStringLiteral("name"));
    const QCommandLineOption positions(QStringLiteral("positions"), QStringLiteral("Channel positions, e.g. FL,FR."), QStringLiteral("list"));
    const QCommandLineOption rate(QStringLiteral("rate"), QStringLiteral("Sample rate."), QStringLiteral("hz"), QStringLiteral("48000"));
    const QCommandLineOption owner(QStringLiteral("owner"), QStringLiteral("The engine's owner tag."), QStringLiteral("tag"));
    const QCommandLineOption description(QStringLiteral("description"), QStringLiteral("The node description."), QStringLiteral("text"));
    const QCommandLineOption bypassed(QStringLiteral("bypassed"), QStringLiteral("Start bypassed."));
    const QCommandLineOption title(QStringLiteral("title"), QStringLiteral("The plugin window's title."), QStringLiteral("text"));
    parser.addOptions({scan, scanClap, scanVst3, format, binary, run, uri, name, positions, rate, owner,
                       description, bypassed, title});
    parser.addHelpOption();
    parser.process(app);

    if (parser.isSet(scanVst3)) {
        writeAll(pp::pluginsToJson(pp::scanVst3Bundle(parser.value(scanVst3).toStdString())) + '\n');
        return 0;
    }
    if (parser.isSet(scanClap)) {
        writeAll(pp::pluginsToJson(pp::scanClapFile(parser.value(scanClap).toStdString())) + '\n');
        return 0;
    }

    pp::Lv2World world;

    if (parser.isSet(scan)) {
        // LV2 here (reading descriptions only); CLAP and VST3 plugins have to
        // be loaded to be read, so each file in a process of its own.
        // Progress as it goes, a line per step: {"progress": {"done", "total",
        // "found"}} (the LV2 plugins are one step, each other file one).
        auto plugins = world.scan();
        std::vector<std::pair<QString, std::string>> files; // option, file
        for (auto &file : pp::clapFiles()) {
            files.emplace_back(QStringLiteral("--scan-clap"), std::move(file));
        }
        for (auto &bundle : pp::vst3Bundles()) {
            files.emplace_back(QStringLiteral("--scan-vst3"), std::move(bundle));
        }
        int done = 1;
        const auto total = static_cast<int>(files.size()) + 1;
        const auto progress = [&] {
            say({{QStringLiteral("progress"),
                  QJsonObject{{QStringLiteral("done"), done},
                              {QStringLiteral("total"), total},
                              {QStringLiteral("found"), static_cast<int>(plugins.size())}}}});
        };
        progress();
        // A few at a time (most of the time goes on waiting for plugins); a
        // batch that hangs is given up on after two minutes.
        constexpr std::size_t together = 4;
        constexpr auto batchTimeout = std::chrono::minutes(2);
        for (std::size_t first = 0; first < files.size(); first += together) {
            std::vector<std::unique_ptr<QProcess>> running;
            const QDeadlineTimer deadline(batchTimeout);
            for (std::size_t i = first; i < std::min(first + together, files.size()); ++i) {
                auto one = std::make_unique<QProcess>();
                one->setProcessChannelMode(QProcess::ForwardedErrorChannel);
                one->start(QCoreApplication::applicationFilePath(),
                           {files[i].first, QString::fromStdString(files[i].second)});
                running.push_back(std::move(one));
            }
            for (std::size_t k = 0; k < running.size(); ++k) {
                QProcess &one = *running[k];
                if (!one.waitForFinished(static_cast<int>(std::max<qint64>(deadline.remainingTime(), 0))) ||
                    one.exitStatus() != QProcess::NormalExit || one.exitCode() != 0) {
                    one.kill();
                    one.waitForFinished(1000);
                    // NOLINTNEXTLINE(modernize-use-std-print): std::print may throw, main mustn't
                    std::fprintf(stderr, "patchprime-pluginhost: %s couldn't be read\n",
                                 files[first + k].second.c_str());
                    continue;
                }
                for (auto &p : pp::pluginsFromJson(one.readAllStandardOutput())) {
                    plugins.push_back(std::move(p));
                }
            }
            done += static_cast<int>(running.size());
            progress();
        }
        writeAll(pp::pluginsToJson(plugins) + '\n');
        return 0;
    }
    if (!parser.isSet(run) || !parser.isSet(uri) || !parser.isSet(name)) {
        parser.showHelp(1);
    }
    // What the plugin starts with, on the first line: {"params", "state"}.
    const auto first = readLine();
    if (!first) {
        return 0; // the engine let go before it began
    }
    const auto init = QJsonDocument::fromJson(*first).object();
    const auto initialParams = init.value(QLatin1String("params")).toObject();
    const auto initialState = init.value(QLatin1String("state")).toString().toStdString();

    const bool clap = parser.value(format) == QLatin1String("clap");
    const bool vst3 = parser.value(format) == QLatin1String("vst3");
    const LilvPlugin *lv2Plugin = nullptr;
    if (!clap && !vst3) {
        lv2Plugin = world.find(parser.value(uri).toStdString());
        if (lv2Plugin == nullptr) {
            return fail(QStringLiteral("the plugin isn't installed"));
        }
        if (const auto problem = world.problem(lv2Plugin); !problem.empty()) {
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
    if (clap || vst3) {
        if (clap) {
            effect = std::make_unique<pp::ClapEffect>(parser.value(binary).toStdString(),
                                                      parser.value(uri).toStdString());
        } else {
            effect = std::make_unique<pp::Vst3Effect>(parser.value(binary).toStdString(),
                                                      parser.value(uri).toStdString());
        }
        if (!effect->error().empty()) {
            return fail(QString::fromStdString(effect->error()));
        }
    } else {
        effect = std::make_unique<pp::Lv2Effect>(world, lv2Plugin);
    }
    pp::HostedPlugin &plugin = *effect;
    for (auto it = initialParams.begin(); it != initialParams.end(); ++it) {
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
    // closes, when the plugin says so, when asked, and at the end).
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
        window = plugin.openWindow(parser.value(title).toStdString(), sayParam);
        if (!window->error().empty()) {
            say({{QStringLiteral("windowError"), QString::fromStdString(window->error())}});
            window.reset();
            say({{QStringLiteral("window"), false}});
            return;
        }
        say({{QStringLiteral("window"), true}});
    };
    QTimer windowTimer;
    windowTimer.setInterval(33);
    QObject::connect(&windowTimer, &QTimer::timeout, &app, [&] {
        if (window && !window->idle()) {
            closeWindow();
        }
        plugin.mainThreadTick();
        plugin.takeParamChanges(sayParam); // ones the plugin reports itself
        if (plugin.wantsRestart()) {
            QCoreApplication::exit(3); // the engine starts it again
        }
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

    // Commands from the engine; the host ends when the engine lets go,
    // sending the plugin's state first.
    QByteArray pending;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    const auto finish = [&] {
        input.setEnabled(false);
        closeWindow();
        sendState();
        QCoreApplication::quit();
    };
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        std::array<char, 4096> buffer{};
        const auto n = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            return; // tried again when there's more
        }
        if (n <= 0) {
            finish();
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
        if (pending.size() > maxLine) {
            finish(); // not the engine talking
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
    // Let the plugin stop processing on the audio thread (some check), then
    // the node goes.
    plugin.stopProcessingSoon();
    for (int i = 0; i < 20 && !plugin.processingStopped(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    graph.stop();
    return code;
}

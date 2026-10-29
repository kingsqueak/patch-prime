// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// Live test against the running PipeWire server: a temporary null sink stands
// in for hardware. Enabling it must create its bus, link the bus monitor to it,
// apply the fader, meter a tone played into the bus, and remove the bus again.

#include "approuter.h"
#include "appstore.h"
#include "busmanager.h"
#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>

using namespace pp;

namespace {

std::optional<QByteArray> run(const QString &program, const QStringList &args)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return std::nullopt;
    }
    return process.readAllStandardOutput().trimmed();
}

// Writes a 3 s stereo 440 Hz tone at half scale (-6 dBFS) as a WAV file.
bool writeTone(const QString &path, int seconds = 3)
{
    constexpr int rate = 48000;
    const auto frames = static_cast<std::uint32_t>(rate * seconds);
    QByteArray data;
    auto put16 = [&data](std::uint16_t v) {
        data.append(static_cast<char>(v & 0xFF));
        data.append(static_cast<char>(v >> 8));
    };
    auto put32 = [&data](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            data.append(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    };
    data.append("RIFF");
    put32(36 + (frames * 4));
    data.append("WAVEfmt ");
    put32(16);
    put16(1);
    put16(2);
    put32(rate);
    put32(rate * 4);
    put16(4);
    put16(16);
    data.append("data");
    put32(frames * 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto sample =
            static_cast<std::int16_t>(16383.0 * std::sin(2.0 * M_PI * 440.0 * i / rate));
        put16(static_cast<std::uint16_t>(sample));
        put16(static_cast<std::uint16_t>(sample));
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

const GraphObject *nodeNamed(const PipeWireGraph &graph, const std::string &name)
{
    for (const auto &[id, node] : graph.nodes()) {
        if (prop(node, "node.name") == name) {
            return &node;
        }
    }
    return nullptr;
}

std::ptrdiff_t linkCount(const PipeWireGraph &graph, const std::string &from,
                         const std::string &to)
{
    const auto *a = nodeNamed(graph, from);
    const auto *b = nodeNamed(graph, to);
    if (a == nullptr || b == nullptr) {
        return 0;
    }
    return std::ranges::count_if(graph.links(), [&](const auto &entry) {
        return prop(entry.second, "link.output.node") == std::to_string(a->id) &&
               prop(entry.second, "link.input.node") == std::to_string(b->id);
    });
}

// A send goes from one bus through its gain node to the other; true when
// both hops have `links` links.
bool sentThrough(const PipeWireGraph &graph, const std::string &from, const std::string &to,
                 const std::string &send, std::ptrdiff_t links)
{
    return linkCount(graph, from, send) == links && linkCount(graph, send, to) == links &&
           linkCount(graph, from, to) == 0;
}

} // namespace

class TestBusLive : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        for (const auto *tool : {"pactl", "pw-dump", "pw-play", "pw-loopback"}) {
            if (QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty()) {
                qWarning("%s not found", tool);
                std::exit(77);
            }
        }
        m_graph.start();
        if (!QTest::qWaitFor([this] { return m_graph.isConnected() && !m_graph.nodes().empty(); },
                             5000)) {
            qWarning("no PipeWire server");
            std::exit(77);
        }
        // The default metadata can arrive after the nodes; without it there'd
        // be nothing to put back in cleanupTestCase().
        [[maybe_unused]] const bool haveDefault =
            QTest::qWaitFor([this] { return !m_graph.defaultSinkName().empty(); }, 2000);
        m_originalDefault = m_graph.defaultSinkName();

        m_sinkName = "pp_bus_test_" + std::to_string(QCoreApplication::applicationPid());
        m_module = QString::fromUtf8(
            run(QStringLiteral("pactl"),
                {QStringLiteral("load-module"), QStringLiteral("module-null-sink"),
                 QStringLiteral("sink_name=") + QString::fromStdString(m_sinkName)})
                .value_or(QByteArray()));
        QVERIFY(!m_module.isEmpty());
    }

    void cleanupTestCase()
    {
        // Put the user's default output back, whatever happened.
        if (!m_originalDefault.empty() && m_graph.defaultSinkName() != m_originalDefault) {
            m_graph.setConfiguredDefault(Direction::Output, m_originalDefault);
            [[maybe_unused]] const bool restored = QTest::qWaitFor(
                [this] { return m_graph.defaultSinkName() == m_originalDefault; }, 3000);
        }
        // Don't leave lingering buses behind, even when a check failed.
        if (!m_busName.empty()) {
            if (const auto *bus = nodeNamed(m_graph, m_busName)) {
                m_graph.destroyObject(bus->id);
                QTest::qWait(200);
            }
        }
        if (!m_module.isEmpty()) {
            run(QStringLiteral("pactl"), {QStringLiteral("unload-module"), m_module});
        }
        // Anything the routing test's engine left behind.
        for (const auto &[id, node] : m_graph.nodes()) {
            if (!m_owner.empty() && prop(node, "patchprime.owner") == m_owner) {
                m_graph.destroyObject(id);
            }
        }
        QTest::qWait(200);
    }

    void busFollowsSettings()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);

        const auto key = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(key) >= 0; }, 5000));

        // Enable: a bus appears, named from the busId, linked to the sink.
        devices.setEnabled(devices.rowForKey(key), true);
        m_busName = busNodeName(Direction::Output, store.find(key)->busId);
        QVERIFY(QTest::qWaitFor([&] { return nodeNamed(m_graph, m_busName) != nullptr; }, 5000));
        const auto busId = nodeNamed(m_graph, m_busName)->id;
        QCOMPARE(prop(*nodeNamed(m_graph, m_busName), "patchprime.key"), key);

        const auto sinkId = std::to_string(nodeNamed(m_graph, m_sinkName)->id);
        const auto busIdText = std::to_string(busId);
        auto linksToSink = [&] {
            return std::ranges::count_if(m_graph.links(), [&](const auto &entry) {
                return prop(entry.second, "link.output.node") == busIdText &&
                       prop(entry.second, "link.input.node") == sinkId;
            });
        };
        QVERIFY(QTest::qWaitFor([&] { return linksToSink() == 2; }, 5000));

        // Fader and mute land on the bus's Props.
        devices.setVolume(devices.rowForKey(key), -6.0);
        devices.setMuted(devices.rowForKey(key), true);
        auto props = [&]() -> QJsonObject {
            const auto dump = run(QStringLiteral("pw-dump"), {QString::number(busId)});
            for (const auto &object :
                 QJsonDocument::fromJson(dump.value_or(QByteArray())).array()) {
                for (const auto &p : object.toObject()
                                         .value(QLatin1String("info"))
                                         .toObject()
                                         .value(QLatin1String("params"))
                                         .toObject()
                                         .value(QLatin1String("Props"))
                                         .toArray()) {
                    if (p.toObject().contains(QLatin1String("channelVolumes"))) {
                        return p.toObject();
                    }
                }
            }
            return {};
        };
        QVERIFY(
            QTest::qWaitFor([&] { return props().value(QLatin1String("mute")).toBool(); }, 5000));
        const auto volumes = props().value(QLatin1String("channelVolumes")).toArray();
        QCOMPARE(volumes.size(), 2);
        QVERIFY(std::fabs(volumes.at(0).toDouble() - 0.501187) < 1e-4);

        // Meters: a -6 dBFS tone through the bus at -6 dB reads about 0.25.
        devices.setMuted(devices.rowForKey(key), false);
        buses.setMetering(true);
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        QProcess player;
        player.start(QStringLiteral("pw-play"),
                     {QStringLiteral("--target"), QString::fromStdString(m_busName), tone});
        float peak = 0.0F;
        QVERIFY(QTest::qWaitFor(
            [&] {
                for (const auto &[k, levels] : buses.takeLevels()) {
                    for (const float level : levels) {
                        peak = std::max(peak, level);
                    }
                }
                return peak > 0.2F;
            },
            5000));
        player.kill();
        player.waitForFinished();
        QVERIFY2(peak < 0.3F, qPrintable(QString::number(peak)));
        buses.setMetering(false);

        // The bus as system default output.
        QVERIFY(m_graph.setConfiguredDefault(Direction::Output, m_busName));
        QVERIFY(QTest::qWaitFor([&] { return m_graph.defaultSinkName() == m_busName; }, 5000));
        QVERIFY(devices.data(devices.index(devices.rowForKey(key)), DeviceModel::BusDefaultRole)
                    .toBool());

        // Disable: the bus goes, and its links with it. Being the default, it
        // first hands the default back to its device.
        devices.setEnabled(devices.rowForKey(key), false);
        QVERIFY(QTest::qWaitFor([&] { return nodeNamed(m_graph, m_busName) == nullptr; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return m_graph.defaultSinkName() == m_sinkName; }, 5000));
        QCOMPARE(linksToSink(), 0);
        m_busName.clear();
    }

    void routesVirtualInput()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owner = busOwnerFor(store.path());

        const auto desktopKey =
            devices.createVirtual(QStringLiteral("Test Desktop"), EndpointKind::VirtualInput);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Test Mix"), EndpointKind::VirtualSink);
        devices.setSend(devices.rowForKey(desktopKey.toStdString()), mixKey, true);

        const auto desktopNode = virtualNodeName(store.find(desktopKey.toStdString())->busId);
        const auto mixNode = virtualNodeName(store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor(
            [&] { return nodeNamed(m_graph, desktopNode) && nodeNamed(m_graph, mixNode); }, 5000));
        QCOMPARE(prop(*nodeNamed(m_graph, desktopNode), "media.class"), "Audio/Sink");
        const auto desktopSend = sendNodeName(store.find(desktopKey.toStdString())->busId,
                                              store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor(
            [&] { return sentThrough(m_graph, desktopNode, mixNode, desktopSend, 2); }, 5000));
        // The send node is hidden from Plasma and pactl: its own media class.
        QCOMPARE(prop(*nodeNamed(m_graph, desktopSend), "media.class"), sendMediaClass);

        // Console style: the mix bus goes on to an output (the test sink).
        const auto outKey = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(outKey) >= 0; }, 5000));
        devices.setEnabled(devices.rowForKey(outKey), true);
        devices.setSend(devices.rowForKey(mixKey.toStdString()), QString::fromStdString(outKey),
                        true);
        const auto outBus = busNodeName(Direction::Output, store.find(outKey)->busId);
        const auto mixSend =
            sendNodeName(store.find(mixKey.toStdString())->busId, store.find(outKey)->busId);
        QVERIFY(QTest::qWaitFor([&] { return sentThrough(m_graph, mixNode, outBus, mixSend, 2); },
                                5000));

        // An app plays into the virtual input; it reaches the mix and the output.
        buses.setMetering(true);
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        QProcess player;
        player.start(QStringLiteral("pw-play"),
                     {QStringLiteral("--target"), QString::fromStdString(desktopNode), tone});
        float mixPeak = 0.0F;
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto levels = buses.takeLevels();
                if (const auto it = levels.find(mixKey.toStdString()); it != levels.end()) {
                    for (const float level : it->second) {
                        mixPeak = std::max(mixPeak, level);
                    }
                }
                return mixPeak > 0.3F;
            },
            5000));
        float outPeak = 0.0F;
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto levels = buses.takeLevels();
                if (const auto it = levels.find(outKey); it != levels.end()) {
                    for (const float level : it->second) {
                        outPeak = std::max(outPeak, level);
                    }
                }
                return outPeak > 0.3F;
            },
            5000));
        player.kill();
        player.waitForFinished();
        buses.setMetering(false);

        devices.setEnabled(devices.rowForKey(outKey), false);
        devices.forget(devices.rowForKey(desktopKey.toStdString()));
        devices.forget(devices.rowForKey(mixKey.toStdString()));
        QVERIFY(QTest::qWaitFor(
            [&] { return !nodeNamed(m_graph, desktopNode) && !nodeNamed(m_graph, mixNode); },
            5000));
    }

    void routesInputToVirtualSink()
    {
        // A fake microphone: pw-loopback turns what plays into pp_feed_<pid>
        // into the source pp_src_<pid>.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_feed_" + pid;
        const auto mic = "pp_src_" + pid;
        QProcess loopback;
        loopback.start(QStringLiteral("pw-loopback"),
                       {QString::fromStdString("--capture-props=media.class=Audio/Sink node.name=" +
                                               feed + " audio.position=[FL,FR]"),
                        QString::fromStdString("--playback-props=media.class=Audio/Source "
                                               "node.name=" +
                                               mic + " audio.position=[FL,FR]")});
        auto stopLoopback = qScopeGuard([&loopback] {
            loopback.terminate();
            loopback.waitForFinished(2000);
        });

        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owner = busOwnerFor(store.path());

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        devices.setEnabled(devices.rowForKey(micKey), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Test Mix"), EndpointKind::VirtualSink);
        devices.setSend(devices.rowForKey(micKey), mixKey, true);

        const auto inBus = busNodeName(Direction::Input, store.find(micKey)->busId);
        const auto mixNode = virtualNodeName(store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor(
            [&] { return nodeNamed(m_graph, inBus) && nodeNamed(m_graph, mixNode); }, 5000));
        QCOMPARE(prop(*nodeNamed(m_graph, inBus), "media.class"), "Audio/Source/Virtual");
        QCOMPARE(prop(*nodeNamed(m_graph, mixNode), "node.description"), "Test Mix");

        const auto send =
            sendNodeName(store.find(micKey)->busId, store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, mic, inBus) == 2; }, 5000));
        QVERIFY(
            QTest::qWaitFor([&] { return sentThrough(m_graph, inBus, mixNode, send, 2); }, 5000));

        // Sound goes all the way: tone -> fake mic -> input bus -> Test Mix.
        buses.setMetering(true);
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        QProcess player;
        player.start(QStringLiteral("pw-play"),
                     {QStringLiteral("--target"), QString::fromStdString(feed), tone});
        float mixPeak = 0.0F;
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto levels = buses.takeLevels();
                if (const auto it = levels.find(mixKey.toStdString()); it != levels.end()) {
                    for (const float level : it->second) {
                        mixPeak = std::max(mixPeak, level);
                    }
                }
                return mixPeak > 0.3F;
            },
            5000));
        player.kill();
        player.waitForFinished();

        // The send level: -20 dB into the mix (the tone peaks at about -6 dBFS).
        devices.setSendLevel(devices.rowForKey(micKey), mixKey, -20.0);
        QTest::qWait(300);
        buses.takeLevels();
        player.start(QStringLiteral("pw-play"),
                     {QStringLiteral("--target"), QString::fromStdString(feed), tone});
        mixPeak = 0.0F;
        // Collects for the whole time: the peak is checked afterwards.
        [[maybe_unused]] const bool never = QTest::qWaitFor(
            [&] {
                if (const auto levels = buses.takeLevels();
                    levels.contains(mixKey.toStdString())) {
                    for (const float level : levels.at(mixKey.toStdString())) {
                        mixPeak = std::max(mixPeak, level);
                    }
                }
                return false;
            },
            1500);
        player.kill();
        player.waitForFinished();
        buses.setMetering(false);
        QVERIFY2(mixPeak > 0.02F && mixPeak < 0.1F, qPrintable(QString::number(mixPeak)));
        QCOMPARE(store.find(micKey)->sendLevels.at(mixKey.toStdString()), -20.0);

        // Mono: the input bus is rebuilt with one channel; both mic channels
        // feed it, and it feeds both channels of the mix.
        devices.setMono(devices.rowForKey(micKey), true);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto *bus = nodeNamed(m_graph, inBus);
                return bus != nullptr && prop(*bus, "audio.channels") == "1" &&
                       linkCount(m_graph, mic, inBus) == 2 &&
                       linkCount(m_graph, inBus, send) == 1 && // the send node is mono too
                       linkCount(m_graph, send, mixNode) == 2;
            },
            5000));

        // Unsend: the links go. Delete the mix and disable the mic: all gone.
        devices.setSend(devices.rowForKey(micKey), mixKey, false);
        QVERIFY(QTest::qWaitFor([&] { return nodeNamed(m_graph, send) == nullptr; }, 5000));
        devices.forget(devices.rowForKey(mixKey.toStdString()));
        devices.setEnabled(devices.rowForKey(micKey), false);
        QVERIFY(QTest::qWaitFor(
            [&] { return !nodeNamed(m_graph, inBus) && !nodeNamed(m_graph, mixNode); }, 5000));
    }

    void usesOneChannelPair()
    {
        // A 6-channel card, like an onboard card in pro-audio mode.
        const auto name = "pp_six_" + std::to_string(QCoreApplication::applicationPid());
        const auto module = QString::fromUtf8(
            run(QStringLiteral("pactl"),
                {QStringLiteral("load-module"), QStringLiteral("module-null-sink"),
                 QStringLiteral("sink_name=") + QString::fromStdString(name),
                 QStringLiteral("channels=6"),
                 QStringLiteral("channel_map=aux0,aux1,aux2,aux3,aux4,aux5")})
                .value_or(QByteArray())
                .trimmed());
        QVERIFY(!module.isEmpty());
        auto unload = qScopeGuard(
            [&module] { run(QStringLiteral("pactl"), {QStringLiteral("unload-module"), module}); });

        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owner = busOwnerFor(store.path());

        const auto key = makeEndpointKey("Audio/Sink", name);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(key) >= 0; }, 5000));
        devices.setChannelPair(devices.rowForKey(key), 2); // channels 3-4
        devices.setEnabled(devices.rowForKey(key), true);
        const auto bus = busNodeName(Direction::Output, store.find(key)->busId);

        // Which of the card's channels the bus feeds.
        auto fedChannels = [&] {
            std::set<std::string> channels;
            const auto *from = nodeNamed(m_graph, bus);
            const auto *to = nodeNamed(m_graph, name);
            if (from == nullptr || to == nullptr) {
                return channels;
            }
            for (const auto &[id, link] : m_graph.links()) {
                if (prop(link, "link.output.node") == std::to_string(from->id) &&
                    prop(link, "link.input.node") == std::to_string(to->id)) {
                    const auto port = m_graph.ports().find(
                        static_cast<std::uint32_t>(std::stoul(prop(link, "link.input.port"))));
                    if (port != m_graph.ports().end()) {
                        channels.insert(prop(port->second, "audio.channel"));
                    }
                }
            }
            return channels;
        };
        QVERIFY(QTest::qWaitFor(
            [&] { return fedChannels() == std::set<std::string>{"AUX2", "AUX3"}; }, 5000));
        QCOMPARE(prop(*nodeNamed(m_graph, bus), "audio.channels"), "2");

        // Another pair: the old links go, the bus stays.
        devices.setChannelPair(devices.rowForKey(key), 1);
        QVERIFY(QTest::qWaitFor(
            [&] { return fedChannels() == std::set<std::string>{"AUX0", "AUX1"}; }, 5000));

        // All channels: a 6-channel bus.
        devices.setChannelPair(devices.rowForKey(key), 0);
        QVERIFY(QTest::qWaitFor([&] { return fedChannels().size() == 6; }, 5000));
        QCOMPARE(prop(*nodeNamed(m_graph, bus), "audio.channels"), "6");

        devices.setEnabled(devices.rowForKey(key), false);
        QVERIFY(QTest::qWaitFor([&] { return nodeNamed(m_graph, bus) == nullptr; }, 5000));
    }

    void routesApps()
    {
        QTemporaryDir dir;
        AppStore store(dir.filePath(QStringLiteral("apps.json")));
        // Scoped, so a running daemon leaves the test app alone.
        const auto scope = "test-" + std::to_string(QCoreApplication::applicationPid());
        AppRouter router(&m_graph, &store, scope);
        const std::string key = "playback|patchprime-test";
        // An earlier test hands the default to the test sink; "the system
        // default" has to be somewhere else here.
        if (!m_originalDefault.empty() && m_originalDefault != m_sinkName) {
            m_graph.setConfiguredDefault(Direction::Output, m_originalDefault);
            QVERIFY(QTest::qWaitFor(
                [this] { return m_graph.defaultSinkName() == m_originalDefault; }, 3000));
        }

        // A silent app (volume 0: one step plays to the real default output).
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone, 20));
        auto startApp = [&tone, &scope](QProcess &player) {
            player.start(QStringLiteral("pw-play"),
                         {QStringLiteral("--volume=0"), QStringLiteral("-P"),
                          QStringLiteral("{ application.name = \"Patch Prime Test\" "
                                         "application.process.binary = \"patchprime-test\" "
                                         "patchprime.scope = \"%1\" }")
                              .arg(QString::fromStdString(scope)),
                          tone});
        };
        auto *player = new QProcess(this);
        auto stopPlayer = qScopeGuard([&player] {
            player->kill();
            player->waitForFinished();
            delete player;
        });
        startApp(*player);

        auto streamId = [&]() -> std::optional<std::uint32_t> {
            for (const auto &[id, node] : m_graph.nodes()) {
                if (const auto app = classifyAppStream(node); app && app->key == key) {
                    return id;
                }
            }
            return std::nullopt;
        };
        auto linkedTo = [&](const std::string &target) {
            const auto stream = streamId();
            const auto *node = nodeNamed(m_graph, target);
            if (!stream || node == nullptr) {
                return false;
            }
            return std::ranges::any_of(m_graph.links(), [&](const auto &entry) {
                return prop(entry.second, "link.output.node") == std::to_string(*stream) &&
                       prop(entry.second, "link.input.node") == std::to_string(node->id);
            });
        };

        // Remembered once it has settled.
        QVERIFY(QTest::qWaitFor([&] { return store.find(key) != nullptr; }, 5000));
        QCOMPARE(store.find(key)->name, "Patch Prime Test");

        // Route it to the test sink, then back to the default.
        QVERIFY(router.setTarget(key, m_sinkName));
        QVERIFY(QTest::qWaitFor([&] { return linkedTo(m_sinkName); }, 5000));
        QVERIFY(router.setTarget(key, {}));
        QVERIFY(QTest::qWaitFor([&] { return !linkedTo(m_sinkName); }, 5000));

        // A move made elsewhere (as Plasma does it) becomes the route.
        const auto serial = prop(*nodeNamed(m_graph, m_sinkName), "object.serial");
        m_graph.setStreamTarget(*streamId(), serial);
        QVERIFY(QTest::qWaitFor([&] { return store.find(key)->target == m_sinkName; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return linkedTo(m_sinkName); }, 5000));

        // Restart the app: the route comes back.
        player->kill();
        player->waitForFinished();
        QVERIFY(QTest::qWaitFor([&] { return !streamId(); }, 5000));
        startApp(*player);
        QVERIFY(QTest::qWaitFor([&] { return linkedTo(m_sinkName); }, 5000));

        // Leave WirePlumber's saved target for the test app at the default.
        QVERIFY(router.setTarget(key, {}));
        QVERIFY(QTest::qWaitFor([&] { return !linkedTo(m_sinkName); }, 5000));
    }

private:
    PipeWireGraph m_graph;
    std::string m_sinkName;
    std::string m_busName;
    std::string m_originalDefault;
    std::string m_owner;
    QString m_module;
};

QTEST_GUILESS_MAIN(TestBusLive)
#include "test_buslive.moc"

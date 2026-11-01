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
#include "plugins.h"
#include "recordingmanager.h"

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
#include <vector>

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

std::ptrdiff_t linkCount(const PipeWireGraph &graph, const std::string &from, const std::string &to)
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

// The ids of the links between two nodes.
std::set<std::uint32_t> linkIds(const PipeWireGraph &graph, const std::string &from,
                                const std::string &to)
{
    std::set<std::uint32_t> ids;
    const auto *a = nodeNamed(graph, from);
    const auto *b = nodeNamed(graph, to);
    if (a == nullptr || b == nullptr) {
        return ids;
    }
    for (const auto &[id, link] : graph.links()) {
        if (prop(link, "link.output.node") == std::to_string(a->id) &&
            prop(link, "link.input.node") == std::to_string(b->id)) {
            ids.insert(id);
        }
    }
    return ids;
}

// Whether the links between these pairs of nodes stay the same links for a
// while: not removed and made again by every reconcile (which drops audio).
bool linksHold(const PipeWireGraph &graph,
               const std::vector<std::pair<std::string, std::string>> &pairs, int ms = 800)
{
    std::vector<std::set<std::uint32_t>> before;
    for (const auto &[from, to] : pairs) {
        before.push_back(linkIds(graph, from, to));
        if (before.back().empty()) {
            return false;
        }
    }
    QTest::qWait(ms);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (linkIds(graph, pairs[i].first, pairs[i].second) != before[i]) {
            return false;
        }
    }
    return true;
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
        [[maybe_unused]] const bool haveDefault = QTest::qWaitFor(
            [this] {
                return !m_graph.defaultSinkName().empty() && !m_graph.defaultSourceName().empty();
            },
            2000);
        m_originalDefault = m_graph.defaultSinkName();
        m_originalSource = m_graph.defaultSourceName();

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
        // Put the user's defaults back, whatever happened. Always written: a
        // test device that has gone leaves PipeWire's effective default
        // looking right while the configured one still names it.
        if (!m_originalSource.empty()) {
            m_graph.setConfiguredDefault(Direction::Input, m_originalSource);
            [[maybe_unused]] const bool restored = QTest::qWaitFor(
                [this] { return m_graph.defaultSourceName() == m_originalSource; }, 3000);
        }
        if (!m_originalDefault.empty()) {
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
        // Anything the tests' engines left behind (each test has its own
        // settings file, so its own owner tag). Send nodes, for one, can
        // outlive a test's BusManager by a reconcile.
        for (const auto &[id, node] : m_graph.nodes()) {
            if (m_owners.contains(prop(node, "patchprime.owner"))) {
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
        m_owners.insert(busOwnerFor(store.path()));

        const auto key = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(key) >= 0; }, 5000));

        // Enable: a bus appears, named from the busId, linked to the sink.
        devices.setEnabled(devices.rowForKey(key), true);
        m_busName = store.find(key)->busName;
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

        // A duplicate of the bus (as a reconnect racing the listing once
        // made): the engine removes the extra and keeps the original, links
        // and all.
        const auto &row = devices.rows()[static_cast<std::size_t>(devices.rowForKey(key))];
        QVERIFY(m_graph.createNode(
            busProperties(row.endpoint, *store.find(key), busOwnerFor(store.path()))));
        auto copies = [&] {
            return std::ranges::count_if(m_graph.nodes(), [&](const auto &entry) {
                return prop(entry.second, "node.name") == m_busName;
            });
        };
        QVERIFY(QTest::qWaitFor([&] { return copies() == 2; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return copies() == 1; }, 5000));
        QCOMPARE(nodeNamed(m_graph, m_busName)->id, busId);
        QCOMPARE(linksToSink(), 2);

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
                     {QStringLiteral("-P"),
                      QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                      QStringLiteral("--target"), QString::fromStdString(m_busName), tone});
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

    void clockModes()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));

        const auto key = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(key) >= 0; }, 5000));
        devices.setEnabled(devices.rowForKey(key), true);
        const auto bus = store.find(key)->busName;
        auto driverPriority = [&] {
            const auto *node = nodeNamed(m_graph, bus);
            return node != nullptr ? prop(*node, "priority.driver") : std::string{};
        };

        // The bus outranks any hardware as the graph driver in both clock
        // modes, so changing the mode doesn't rebuild it: a chosen master
        // interface outranks it through WirePlumber rules instead, and when
        // that interface is unplugged the buses take over by themselves.
        QVERIFY(QTest::qWaitFor([&] { return driverPriority() == "20000"; }, 5000));
        const auto busId = nodeNamed(m_graph, bus)->id;
        devices.setClock(ClockMode::Hardware);
        QTest::qWait(300);
        QCOMPARE(store.clock(), ClockMode::Hardware);
        QVERIFY(nodeNamed(m_graph, bus) != nullptr);
        QCOMPARE(nodeNamed(m_graph, bus)->id, busId); // same node: not rebuilt
        QCOMPARE(driverPriority(), "20000");
        QCOMPARE(linkCount(m_graph, bus, m_sinkName), 2);

        devices.setEnabled(devices.rowForKey(key), false);
        QVERIFY(QTest::qWaitFor([&] { return nodeNamed(m_graph, bus) == nullptr; }, 5000));
    }

    void routesVirtualInput()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));

        const auto desktopKey =
            devices.createVirtual(QStringLiteral("Test Desktop"), EndpointKind::VirtualInput);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Test Mix"), EndpointKind::VirtualSink);
        devices.setSend(devices.rowForKey(desktopKey.toStdString()), mixKey, true);

        const auto desktopNode = store.find(desktopKey.toStdString())->busName;
        const auto mixNode = store.find(mixKey.toStdString())->busName;
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
        const auto outBus = store.find(outKey)->busName;
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
                     {QStringLiteral("-P"),
                      QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                      QStringLiteral("--target"), QString::fromStdString(desktopNode), tone});
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

    // Records a mix bus while a tone plays into it, then plays the file
    // into a routed input.
    void recordsAndPlays()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));
        RecordingManager recordings(&m_graph, &devices, &buses, dir.filePath(QStringLiteral("rec")));

        const auto mixKey =
            devices.createVirtual(QStringLiteral("Test Mix"), EndpointKind::VirtualSink).toStdString();
        const auto desktopKey =
            devices.createVirtual(QStringLiteral("Test Desktop"), EndpointKind::VirtualInput)
                .toStdString();
        const auto mixNode = store.find(mixKey)->busName;
        const auto desktopNode = store.find(desktopKey)->busName;
        QVERIFY(QTest::qWaitFor(
            [&] { return nodeNamed(m_graph, mixNode) && nodeNamed(m_graph, desktopNode); }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(mixKey) >= 0; }, 5000));

        // Only mix buses and routed inputs record.
        const auto outKey = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(outKey) >= 0; }, 5000));
        QString error;
        QVERIFY(!recordings.start(outKey, &error));
        QVERIFY(!error.isEmpty());

        recordings.setFormat(RecordingFormat::Flac);
        QVERIFY2(recordings.start(mixKey, &error), qPrintable(error));
        const auto recorderNode = std::string(RecordingManager::recorderPrefix) + mixNode.substr(11);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, mixNode, recorderNode) == 2; },
                                5000));
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        QProcess player;
        player.start(QStringLiteral("pw-play"),
                     {QStringLiteral("-P"),
                      QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                      QStringLiteral("--target"), QString::fromStdString(mixNode), tone});
        QVERIFY(QTest::qWaitFor([&] { return recordings.recordedSeconds()[mixKey] > 2.0; }, 8000));
        player.kill();
        player.waitForFinished();
        const auto path = recordings.recording().at(mixKey);
        recordings.stop(mixKey);
        QVERIFY(recordings.recording().empty());
        QVERIFY(QTest::qWaitFor([&] { return !nodeNamed(m_graph, recorderNode); }, 3000));
        const auto files = recordings.files();
        QCOMPARE(files.size(), std::size_t{1});
        QCOMPARE(files.front().path, path);
        QVERIFY(files.front().name.startsWith(QStringLiteral("Test Mix ")));
        QVERIFY(files.front().seconds > 2.0);

        // The tone (-6 dBFS) is in the file.
        {
            const auto file = Player::open(path, &error);
            QVERIFY2(file, qPrintable(error));
            std::vector<float> buffer(512);
            float peak = 0.0F;
            for (int i = 0; i < 2000 && !file->finished(); ++i) {
                file->process(buffer.data(), 256, 2);
                for (const float x : buffer) {
                    peak = std::max(peak, std::abs(x));
                }
                QTest::qWait(0);
            }
            QVERIFY2(std::abs(peak - 0.5F) < 0.05F, qPrintable(QString::number(peak)));
        }

        // Played into the routed input, it shows on that strip's meter.
        buses.setMetering(true);
        recordings.setPlayerTarget(desktopKey);
        QVERIFY2(recordings.play(path, &error), qPrintable(error));
        QVERIFY(QTest::qWaitFor(
            [&] { return linkCount(m_graph, std::string(RecordingManager::playerName), desktopNode) == 2; },
            5000));
        float peak = 0.0F;
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto levels = buses.takeLevels();
                if (const auto it = levels.find(desktopKey); it != levels.end()) {
                    for (const float level : it->second) {
                        peak = std::max(peak, level);
                    }
                }
                return peak > 0.3F;
            },
            5000));
        QVERIFY(recordings.player().position > 0.0);
        // Moving it moves the links.
        recordings.setPlayerTarget(mixKey);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, std::string(RecordingManager::playerName), mixNode) == 2 &&
                       linkCount(m_graph, std::string(RecordingManager::playerName), desktopNode) == 0;
            },
            5000));
        recordings.stopPlayback();
        QVERIFY(recordings.player().path.isEmpty());
        buses.setMetering(false);

        devices.forget(devices.rowForKey(mixKey));
        devices.forget(devices.rowForKey(desktopKey));
        QVERIFY(QTest::qWaitFor(
            [&] { return !nodeNamed(m_graph, mixNode) && !nodeNamed(m_graph, desktopNode); }, 5000));
    }

    // A raw sample of a mic, played back into that mic's strip (looping)
    // in place of the device, through its effects.
    void playsIntoAnInput()
    {
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_rfeed_" + pid;
        const auto mic = "pp_rsrc_" + pid;
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
        m_owners.insert(busOwnerFor(store.path()));
        RecordingManager recordings(&m_graph, &devices, &buses, dir.filePath(QStringLiteral("rec")));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        devices.setEnabled(devices.rowForKey(micKey), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Test Mix"), EndpointKind::VirtualSink);
        devices.setSend(devices.rowForKey(micKey), mixKey, true);
        devices.addEffect(devices.rowForKey(micKey), QStringLiteral("gain"));
        devices.setEffectParam(devices.rowForKey(micKey), 0, QStringLiteral("gainDb"), -20.0);
        const auto inBus = store.find(micKey)->busName;
        const auto micFx = effectsNodeName(inBus);
        const auto mixNode = store.find(mixKey.toStdString())->busName;
        QVERIFY(QTest::qWaitFor(
            [&] { return linkCount(m_graph, mic, micFx) == 2 && nodeNamed(m_graph, mixNode); },
            5000));

        // Record the mic raw: straight from the device, before the -20 dB.
        QString error;
        QVERIFY2(recordings.start(micKey, &error), qPrintable(error));
        const auto recorderNode = std::string(RecordingManager::recorderPrefix) + inBus.substr(11);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, mic, recorderNode) == 2; }, 5000));
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        QProcess tonePlayer;
        tonePlayer.start(QStringLiteral("pw-play"),
                         {QStringLiteral("-P"),
                          QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                          QStringLiteral("--target"), QString::fromStdString(feed), tone});
        QVERIFY(QTest::qWaitFor([&] { return recordings.recordedSeconds()[micKey] > 1.5; }, 8000));
        tonePlayer.kill();
        tonePlayer.waitForFinished();
        const auto path = recordings.recording().at(micKey);
        recordings.stop(micKey);
        {
            const auto file = Player::open(path, &error);
            QVERIFY2(file, qPrintable(error));
            std::vector<float> buffer(512);
            float peak = 0.0F;
            for (int i = 0; i < 2000 && !file->finished(); ++i) {
                file->process(buffer.data(), 256, 2);
                for (const float x : buffer) {
                    peak = std::max(peak, std::abs(x));
                }
                QTest::qWait(0);
            }
            QVERIFY2(std::abs(peak - 0.5F) < 0.05F, qPrintable(QString::number(peak))); // raw
        }

        // Played back into the mic's strip: the device is cut off, the
        // recording goes through the effects (-20 dB) into the mix.
        buses.setMetering(true);
        recordings.setLoop(true);
        recordings.setPlayerTarget(micKey);
        QVERIFY2(recordings.play(path, &error), qPrintable(error));
        const auto player = std::string(RecordingManager::playerName);
        QVERIFY(QTest::qWaitFor(
            [&] { return linkCount(m_graph, player, micFx) == 2 && linkCount(m_graph, mic, micFx) == 0; },
            5000));
        float mixPeak = 0.0F;
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto levels = buses.takeLevels();
                if (const auto it = levels.find(mixKey.toStdString()); it != levels.end()) {
                    for (const float level : it->second) {
                        mixPeak = std::max(mixPeak, level);
                    }
                }
                return mixPeak > 0.04F;
            },
            5000));
        QVERIFY2(mixPeak < 0.07F, qPrintable(QString::number(mixPeak)));
        // It loops: still playing well past its length.
        const auto length = recordings.player().duration;
        QTest::qWait(static_cast<int>((length + 1.0) * 1000));
        QVERIFY(!recordings.player().path.isEmpty());

        // Stopped, the mic comes back.
        recordings.stopPlayback();
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, mic, micFx) == 2; }, 5000));
        QCOMPARE(linkCount(m_graph, player, micFx), 0);
        buses.setMetering(false);

        devices.forget(devices.rowForKey(mixKey.toStdString()));
        devices.setEnabled(devices.rowForKey(micKey), false);
        QVERIFY(QTest::qWaitFor([&] { return !nodeNamed(m_graph, inBus); }, 5000));
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
        m_owners.insert(busOwnerFor(store.path()));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        devices.setEnabled(devices.rowForKey(micKey), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Test Mix"), EndpointKind::VirtualSink);
        devices.setSend(devices.rowForKey(micKey), mixKey, true);

        const auto inBus = store.find(micKey)->busName;
        const auto mixNode = store.find(mixKey.toStdString())->busName;
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
                     {QStringLiteral("-P"),
                      QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                      QStringLiteral("--target"), QString::fromStdString(feed), tone});
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
                     {QStringLiteral("-P"),
                      QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                      QStringLiteral("--target"), QString::fromStdString(feed), tone});
        mixPeak = 0.0F;
        // Collects for the whole time: the peak is checked afterwards.
        [[maybe_unused]] const bool never = QTest::qWaitFor(
            [&] {
                if (const auto levels = buses.takeLevels(); levels.contains(mixKey.toStdString())) {
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

        // The input bus as the system microphone; disabling the strip hands
        // the default back to the (fake) mic itself. Only when there's a
        // default microphone to put back afterwards.
        if (m_originalSource.empty()) {
            devices.setEnabled(devices.rowForKey(micKey), false);
        } else {
            m_graph.setConfiguredDefault(Direction::Input, inBus);
            QVERIFY(QTest::qWaitFor([&] { return m_graph.defaultSourceName() == inBus; }, 5000));
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return devices
                        .data(devices.index(devices.rowForKey(micKey)), DeviceModel::BusDefaultRole)
                        .toBool();
                },
                5000));
            devices.setEnabled(devices.rowForKey(micKey), false);
            QVERIFY(QTest::qWaitFor([&] { return m_graph.defaultSourceName() == mic; }, 5000));
            // Put the user's microphone back now: once the fake mic is gone,
            // PipeWire falls back on its own and the stale setting would linger.
            m_graph.setConfiguredDefault(Direction::Input, m_originalSource);
            QVERIFY(QTest::qWaitFor(
                [this] { return m_graph.defaultSourceName() == m_originalSource; }, 3000));
        }
        QVERIFY(QTest::qWaitFor(
            [&] { return !nodeNamed(m_graph, inBus) && !nodeNamed(m_graph, mixNode); }, 5000));
    }

    void runsPlugins()
    {
        // A fake stereo mic sent to a mix bus; a plugin (the test gain, a
        // mono plugin: one instance per channel) in the mic's chain, in a
        // process of its own, alongside a built-in effect.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_plugfeed_" + pid;
        const auto mic = "pp_plugsrc_" + pid;
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
        PluginCatalog catalog(dir.filePath(QStringLiteral("plugins.json")));
        catalog.start();
        QVERIFY(QTest::qWaitFor([&] { return !catalog.isScanning(); }, 15000));
        const std::string gainType = "lv2:https://github.com/kingsqueak/patch-prime/tests/gain";
        QVERIFY2(catalog.find(gainType) != nullptr, "the test plugin wasn't found");

        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.setPluginCatalog(&catalog);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        const auto micRow = [&] { return devices.rowForKey(micKey); };
        devices.setEnabled(micRow(), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Plug Mix"), EndpointKind::VirtualSink);
        devices.setSend(micRow(), mixKey, true);
        const auto inBus = store.find(micKey)->busName;
        const auto mixNode = store.find(mixKey.toStdString())->busName;
        const auto send =
            sendNodeName(store.find(micKey)->busId, store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor([&] { return sentThrough(m_graph, inBus, mixNode, send, 2); }, 5000));

        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        buses.setMetering(true);
        // The mix bus's peak while the tone (peaking near 0.5) plays into the mic.
        const auto peakWhilePlaying = [&](int ms) {
            QProcess player;
            player.start(QStringLiteral("pw-play"),
                         {QStringLiteral("-P"),
                          QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                          QStringLiteral("--target"), QString::fromStdString(feed), tone});
            QTest::qWait(300);
            buses.takeLevels();
            float peak = 0.0F;
            [[maybe_unused]] const bool never = QTest::qWaitFor(
                [&] {
                    if (const auto levels = buses.takeLevels(); levels.contains(mixKey.toStdString())) {
                        for (const float level : levels.at(mixKey.toStdString())) {
                            peak = std::max(peak, level);
                        }
                    }
                    return false;
                },
                ms);
            player.kill();
            player.waitForFinished();
            return peak;
        };

        // The plugin at x0.1 (-20 dB): mic -> plugin -> bus.
        const auto plug = pluginNodeName(inBus, 1);
        devices.addEffect(micRow(), QString::fromStdString(gainType));
        devices.setEffectParam(micRow(), 0, QStringLiteral("gain"), 0.1);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, plug) == 2 && linkCount(m_graph, plug, inBus) == 2 &&
                       linkCount(m_graph, mic, inBus) == 0;
            },
            10000));
        QVERIFY(linksHold(m_graph, {{mic, plug}, {plug, inBus}}));
        QCOMPARE(prop(*nodeNamed(m_graph, plug), "node.autoconnect"), "false");
        // Its levels come from its process, like a built-in's (peaks held
        // until taken).
        buses.setEffectMetering({micKey});
        QTest::qWait(100);
        const float reduced = peakWhilePlaying(1200);
        QVERIFY2(reduced > 0.02F && reduced < 0.1F, qPrintable(QString::number(reduced)));
        const auto levels = buses.takeEffectLevels();
        const auto io = levels.find({micKey, 0});
        QVERIFY(io != levels.end());
        const float fxIn = std::max(io->second.inPeak[0], io->second.inPeak[1]);
        const float fxOut = std::max(io->second.outPeak[0], io->second.outPeak[1]);
        QVERIFY2(fxIn > 0.3F && std::abs((20.0F * std::log10(fxOut / fxIn)) + 20.0F) < 1.0F,
                 qPrintable(QStringLiteral("%1 %2").arg(fxIn).arg(fxOut)));
        buses.setEffectMetering({});

        // A built-in after it, then before it: the series follows the chain.
        const auto fx = effectsNodeName(inBus);
        devices.addEffect(micRow(), QStringLiteral("gain"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, plug) == 2 && linkCount(m_graph, plug, fx) == 2 &&
                       linkCount(m_graph, fx, inBus) == 2 && linkCount(m_graph, plug, inBus) == 0;
            },
            10000));
        devices.moveEffect(micRow(), 1, 0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, fx) == 2 && linkCount(m_graph, fx, plug) == 2 &&
                       linkCount(m_graph, plug, inBus) == 2 && linkCount(m_graph, mic, plug) == 0 &&
                       linkCount(m_graph, fx, inBus) == 0;
            },
            10000));
        QVERIFY(linksHold(m_graph, {{mic, fx}, {fx, plug}, {plug, inBus}}));

        // It crashes (and again, each time it's restarted): the strip
        // carries on without it, and after a few tries it's left off.
        devices.setEffectParam(micRow(), 1, QStringLiteral("crash"), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return !buses.pluginErrors().empty(); }, 15000));
        QCOMPARE(buses.pluginErrors().begin()->first, (std::pair{micKey, std::size_t{1}}));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return nodeNamed(m_graph, plug) == nullptr && linkCount(m_graph, fx, inBus) == 2;
            },
            5000));
        const float without = peakWhilePlaying(1200);
        QVERIFY2(without > 0.3F, qPrintable(QString::number(without)));

        // A changed setting gives it another go.
        devices.setEffectParam(micRow(), 1, QStringLiteral("crash"), 0.0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return buses.pluginErrors().empty() && linkCount(m_graph, fx, plug) == 2 &&
                       linkCount(m_graph, plug, inBus) == 2 && linkCount(m_graph, fx, inBus) == 0;
            },
            20000));
        const float back = peakWhilePlaying(1200);
        QVERIFY2(back > 0.02F && back < 0.1F, qPrintable(QString::number(back)));

        // The plugin's own state: "halve" sets a trim it keeps only there
        // (switching halve off again doesn't undo it). Saved when asked, and
        // restored when the plugin starts again, after another crash.
        devices.setEffectParam(micRow(), 1, QStringLiteral("halve"), 1.0);
        QTest::qWait(200);
        devices.setEffectParam(micRow(), 1, QStringLiteral("halve"), 0.0);
        const float halved = peakWhilePlaying(1200);
        QVERIFY2(halved > 0.012F && halved < 0.035F, qPrintable(QString::number(halved)));
        buses.requestPluginStates();
        QVERIFY(QTest::qWaitFor([&] { return !store.find(micKey)->effects[1].state.empty(); }, 5000));
        QVERIFY(store.find(micKey)->effects[1].state.find("trim") != std::string::npos);
        devices.setEffectParam(micRow(), 1, QStringLiteral("crash"), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return !buses.pluginErrors().empty(); }, 15000));
        devices.setEffectParam(micRow(), 1, QStringLiteral("crash"), 0.0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return buses.pluginErrors().empty() && linkCount(m_graph, fx, plug) == 2 &&
                       linkCount(m_graph, plug, inBus) == 2;
            },
            20000));
        const float restored = peakWhilePlaying(1200);
        QVERIFY2(restored > 0.012F && restored < 0.035F, qPrintable(QString::number(restored)));

        // Without effects the mic goes straight to its bus, and the host goes.
        devices.removeEffect(micRow(), 1);
        devices.removeEffect(micRow(), 0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, inBus) == 2 && nodeNamed(m_graph, plug) == nullptr &&
                       nodeNamed(m_graph, fx) == nullptr;
            },
            10000));
        buses.setMetering(false);
    }

    void pluginStateFollowsTheEffect()
    {
        // Two of one plugin on a strip; the first has its own state (halve's
        // trim). Removing it moves the second into the first's node: that
        // node's host must start again with the second's state, not carry
        // the first's on.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_statefeed_" + pid;
        const auto mic = "pp_statesrc_" + pid;
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
        PluginCatalog catalog(dir.filePath(QStringLiteral("plugins.json")));
        catalog.start();
        QVERIFY(QTest::qWaitFor([&] { return !catalog.isScanning(); }, 30000));
        const std::string gainType = "lv2:https://github.com/kingsqueak/patch-prime/tests/gain";
        QVERIFY(catalog.find(gainType) != nullptr);

        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.setPluginCatalog(&catalog);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        const auto micRow = [&] { return devices.rowForKey(micKey); };
        devices.setEnabled(micRow(), true);
        const auto mixKey = devices.createVirtual(QStringLiteral("State Mix"), EndpointKind::VirtualSink);
        devices.setSend(micRow(), mixKey, true);
        const auto inBus = store.find(micKey)->busName;
        const auto plug1 = pluginNodeName(inBus, 1);
        const auto plug2 = pluginNodeName(inBus, 2);

        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        buses.setMetering(true);
        const auto peakWhilePlaying = [&](int ms) {
            QProcess player;
            player.start(QStringLiteral("pw-play"),
                         {QStringLiteral("-P"),
                          QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                          QStringLiteral("--target"), QString::fromStdString(feed), tone});
            QTest::qWait(300);
            buses.takeLevels();
            float peak = 0.0F;
            [[maybe_unused]] const bool never = QTest::qWaitFor(
                [&] {
                    if (const auto levels = buses.takeLevels(); levels.contains(mixKey.toStdString())) {
                        for (const float level : levels.at(mixKey.toStdString())) {
                            peak = std::max(peak, level);
                        }
                    }
                    return false;
                },
                ms);
            player.kill();
            player.waitForFinished();
            return peak;
        };

        // First: x1 and halved (its state); second: x1, untouched.
        devices.addEffect(micRow(), QString::fromStdString(gainType));
        devices.addEffect(micRow(), QString::fromStdString(gainType));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, plug1) == 2 && linkCount(m_graph, plug1, plug2) == 2 &&
                       linkCount(m_graph, plug2, store.find(micKey)->busName) == 2;
            },
            10000));
        devices.setEffectParam(micRow(), 0, QStringLiteral("halve"), 1.0);
        QTest::qWait(200);
        devices.setEffectParam(micRow(), 0, QStringLiteral("halve"), 0.0);
        buses.requestPluginStates();
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto &effects = store.find(micKey)->effects;
                return !effects[0].state.empty() && !effects[1].state.empty() &&
                       effects[0].state != effects[1].state; // halved, and not
            },
            5000));
        const float halved = peakWhilePlaying(1200);
        QVERIFY2(halved > 0.15F && halved < 0.35F, qPrintable(QString::number(halved)));

        // The halved one goes: what's left (in node 1 now) is at full level.
        devices.removeEffect(micRow(), 0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, plug1) == 2 &&
                       linkCount(m_graph, plug1, inBus) == 2 && nodeNamed(m_graph, plug2) == nullptr;
            },
            15000));
        QVERIFY(linksHold(m_graph, {{mic, plug1}, {plug1, inBus}}));
        const float whole = peakWhilePlaying(1200);
        QVERIFY2(whole > 0.4F, qPrintable(QString::number(whole)));
        buses.setMetering(false);
    }

    void runsClapPlugins()
    {
        // The CLAP test gain on a fake stereo mic (one instance per channel),
        // in its own process: sound through it, a parameter the plugin
        // changes itself coming back to the settings, and its state.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_clapfeed_" + pid;
        const auto mic = "pp_clapsrc_" + pid;
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
        PluginCatalog catalog(dir.filePath(QStringLiteral("plugins.json")));
        catalog.start();
        QVERIFY(QTest::qWaitFor([&] { return !catalog.isScanning(); }, 30000));
        const std::string gainType = "clap:com.github.kingsqueak.patch-prime.tests.gain";
        QVERIFY2(catalog.find(gainType) != nullptr, "the CLAP test plugin wasn't found");

        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.setPluginCatalog(&catalog);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        const auto micRow = [&] { return devices.rowForKey(micKey); };
        devices.setEnabled(micRow(), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Clap Mix"), EndpointKind::VirtualSink);
        devices.setSend(micRow(), mixKey, true);
        const auto inBus = store.find(micKey)->busName;
        const auto mixNode = store.find(mixKey.toStdString())->busName;
        const auto send =
            sendNodeName(store.find(micKey)->busId, store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor([&] { return sentThrough(m_graph, inBus, mixNode, send, 2); }, 5000));

        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        buses.setMetering(true);
        const auto peakWhilePlaying = [&](int ms) {
            QProcess player;
            player.start(QStringLiteral("pw-play"),
                         {QStringLiteral("-P"),
                          QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                          QStringLiteral("--target"), QString::fromStdString(feed), tone});
            QTest::qWait(300);
            buses.takeLevels();
            float peak = 0.0F;
            [[maybe_unused]] const bool never = QTest::qWaitFor(
                [&] {
                    if (const auto levels = buses.takeLevels(); levels.contains(mixKey.toStdString())) {
                        for (const float level : levels.at(mixKey.toStdString())) {
                            peak = std::max(peak, level);
                        }
                    }
                    return false;
                },
                ms);
            player.kill();
            player.waitForFinished();
            return peak;
        };

        const auto plug = pluginNodeName(inBus, 1);
        devices.addEffect(micRow(), QString::fromStdString(gainType));
        devices.setEffectParam(micRow(), 0, QStringLiteral("1"), 0.1); // gain
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, plug) == 2 && linkCount(m_graph, plug, inBus) == 2 &&
                       linkCount(m_graph, mic, inBus) == 0;
            },
            10000));
        QVERIFY(linksHold(m_graph, {{mic, plug}, {plug, inBus}}));
        const float reduced = peakWhilePlaying(1200);
        QVERIFY2(reduced > 0.02F && reduced < 0.1F, qPrintable(QString::number(reduced)));

        // Halve: the plugin halves its output and turns the switch off
        // itself; the settings follow.
        devices.setEffectParam(micRow(), 0, QStringLiteral("3"), 1.0);
        QCOMPARE(store.find(micKey)->effects[0].params.at("3"), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return store.find(micKey)->effects[0].params.at("3") == 0.0; },
                                5000));
        const float halved = peakWhilePlaying(1200);
        QVERIFY2(halved > 0.012F && halved < 0.035F, qPrintable(QString::number(halved)));

        // Its state (the trim) is saved when asked.
        buses.requestPluginStates();
        QVERIFY(QTest::qWaitFor([&] { return !store.find(micKey)->effects[0].state.empty(); }, 5000));

        devices.removeEffect(micRow(), 0);
        QVERIFY(QTest::qWaitFor(
            [&] { return linkCount(m_graph, mic, inBus) == 2 && nodeNamed(m_graph, plug) == nullptr; },
            10000));
        buses.setMetering(false);
    }

    void runsVst3Plugins()
    {
        // The VST3 test gain (mono: the stereo mic is mixed into it) in its
        // own process: sound through it, a parameter the plugin changes
        // itself coming back to the settings, and its state.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_vst3feed_" + pid;
        const auto mic = "pp_vst3src_" + pid;
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
        PluginCatalog catalog(dir.filePath(QStringLiteral("plugins.json")));
        catalog.start();
        QVERIFY(QTest::qWaitFor([&] { return !catalog.isScanning(); }, 30000));
        const std::string gainType = "vst3:50505450546573744761696E56535433";
        QVERIFY2(catalog.find(gainType) != nullptr, "the VST3 test plugin wasn't found");

        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.setPluginCatalog(&catalog);
        devices.attach(&m_graph);
        BusManager buses(&m_graph, &devices);
        m_owners.insert(busOwnerFor(store.path()));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(micKey) >= 0; }, 5000));
        const auto micRow = [&] { return devices.rowForKey(micKey); };
        devices.setEnabled(micRow(), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Vst3 Mix"), EndpointKind::VirtualSink);
        devices.setSend(micRow(), mixKey, true);
        const auto inBus = store.find(micKey)->busName;
        const auto mixNode = store.find(mixKey.toStdString())->busName;
        const auto send =
            sendNodeName(store.find(micKey)->busId, store.find(mixKey.toStdString())->busId);
        QVERIFY(QTest::qWaitFor([&] { return sentThrough(m_graph, inBus, mixNode, send, 2); }, 5000));

        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        buses.setMetering(true);
        const auto peakWhilePlaying = [&](int ms) {
            QProcess player;
            player.start(QStringLiteral("pw-play"),
                         {QStringLiteral("-P"),
                          QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                          QStringLiteral("--target"), QString::fromStdString(feed), tone});
            QTest::qWait(300);
            buses.takeLevels();
            float peak = 0.0F;
            [[maybe_unused]] const bool never = QTest::qWaitFor(
                [&] {
                    if (const auto levels = buses.takeLevels(); levels.contains(mixKey.toStdString())) {
                        for (const float level : levels.at(mixKey.toStdString())) {
                            peak = std::max(peak, level);
                        }
                    }
                    return false;
                },
                ms);
            player.kill();
            player.waitForFinished();
            return peak;
        };

        const auto plug = pluginNodeName(inBus, 1);
        devices.addEffect(micRow(), QString::fromStdString(gainType));
        devices.setEffectParam(micRow(), 0, QStringLiteral("1"), 0.025); // normalized: x0.1
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, plug) == 2 && linkCount(m_graph, plug, inBus) == 2 &&
                       linkCount(m_graph, mic, inBus) == 0;
            },
            10000));
        QVERIFY(linksHold(m_graph, {{mic, plug}, {plug, inBus}}));
        const float reduced = peakWhilePlaying(1200);
        QVERIFY2(reduced > 0.02F && reduced < 0.1F, qPrintable(QString::number(reduced)));

        // Halve: the plugin halves its output and turns the switch off
        // itself; the settings follow.
        devices.setEffectParam(micRow(), 0, QStringLiteral("3"), 1.0);
        QCOMPARE(store.find(micKey)->effects[0].params.at("3"), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return store.find(micKey)->effects[0].params.at("3") == 0.0; },
                                5000));
        const float halved = peakWhilePlaying(1200);
        QVERIFY2(halved > 0.012F && halved < 0.035F, qPrintable(QString::number(halved)));

        // Its state (the trim) is saved when asked.
        buses.requestPluginStates();
        QVERIFY(QTest::qWaitFor([&] { return !store.find(micKey)->effects[0].state.empty(); }, 5000));

        devices.removeEffect(micRow(), 0);
        QVERIFY(QTest::qWaitFor(
            [&] { return linkCount(m_graph, mic, inBus) == 2 && nodeNamed(m_graph, plug) == nullptr; },
            10000));
        buses.setMetering(false);
    }

    void runsEffects()
    {
        // A fake mic sent to a mix bus, and the test sink as an output: each
        // strip with effects runs through its own effects node.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_fxfeed_" + pid;
        const auto mic = "pp_fxsrc_" + pid;
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
        m_owners.insert(busOwnerFor(store.path()));

        const auto micKey = makeEndpointKey("Audio/Source", mic);
        const auto sinkKey = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor(
            [&] { return devices.rowForKey(micKey) >= 0 && devices.rowForKey(sinkKey) >= 0; },
            5000));
        devices.setEnabled(devices.rowForKey(micKey), true);
        devices.setEnabled(devices.rowForKey(sinkKey), true);
        const auto mixKey =
            devices.createVirtual(QStringLiteral("FX Mix"), EndpointKind::VirtualSink);
        devices.setSend(devices.rowForKey(micKey), mixKey, true);
        const auto inBus = store.find(micKey)->busName;
        const auto outBus = store.find(sinkKey)->busName;
        m_busName = outBus;
        const auto mixNode = store.find(mixKey.toStdString())->busName;
        const auto send =
            sendNodeName(store.find(micKey)->busId, store.find(mixKey.toStdString())->busId);
        QVERIFY(
            QTest::qWaitFor([&] { return sentThrough(m_graph, inBus, mixNode, send, 2); }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, outBus, m_sinkName) == 2; }, 5000));

        // -20 dB on the mic: its effects node goes in front of its bus (so
        // apps recording the bus get the effects too); the send is unchanged.
        const auto micFx = effectsNodeName(inBus);
        devices.addEffect(devices.rowForKey(micKey), QStringLiteral("gain"));
        devices.setEffectParam(devices.rowForKey(micKey), 0, QStringLiteral("gainDb"), -20.0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mic, micFx) == 2 && linkCount(m_graph, micFx, inBus) == 2 &&
                       linkCount(m_graph, mic, inBus) == 0 &&
                       sentThrough(m_graph, inBus, mixNode, send, 2);
            },
            5000));
        QVERIFY(linksHold(m_graph, {{mic, micFx}, {micFx, inBus}}));
        const auto *fxNode = nodeNamed(m_graph, micFx);
        QCOMPARE(prop(*fxNode, "media.class"), std::string(effectsMediaClass));
        QCOMPARE(prop(*fxNode, "node.autoconnect"), "false");

        // And sound gets through it, 20 dB down (the tone peaks near 0.5).
        buses.setMetering(true);
        QTemporaryDir toneDir;
        const auto tone = toneDir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(tone));
        // The effect's own input and output levels, while they're watched.
        float fxIn = 0.0F;
        float fxOut = 0.0F;
        const auto peakWhilePlaying = [&](int ms) {
            QProcess player;
            player.start(QStringLiteral("pw-play"),
                         {QStringLiteral("-P"),
                          QStringLiteral("{ patchprime.scope = \"test-untouched\" }"),
                          QStringLiteral("--target"), QString::fromStdString(feed), tone});
            QTest::qWait(300);
            buses.takeLevels();
            float peak = 0.0F;
            [[maybe_unused]] const bool never = QTest::qWaitFor(
                [&] {
                    if (const auto levels = buses.takeLevels();
                        levels.contains(mixKey.toStdString())) {
                        for (const float level : levels.at(mixKey.toStdString())) {
                            peak = std::max(peak, level);
                        }
                    }
                    for (const auto &[at, io] : buses.takeEffectLevels()) {
                        if (at == std::pair{micKey, std::size_t{0}}) {
                            fxIn = std::max({fxIn, io.inPeak[0], io.inPeak[1]});
                            fxOut = std::max({fxOut, io.outPeak[0], io.outPeak[1]});
                        }
                    }
                    return false;
                },
                ms);
            player.kill();
            player.waitForFinished();
            return peak;
        };
        QVERIFY(buses.takeEffectLevels().empty()); // nobody watching
        buses.setEffectMetering({micKey});
        const float reduced = peakWhilePlaying(1200);
        QVERIFY2(reduced > 0.02F && reduced < 0.1F, qPrintable(QString::number(reduced)));
        QVERIFY2(fxIn > 0.3F && std::abs((20.0F * std::log10(fxOut / fxIn)) + 20.0F) < 1.0F,
                 qPrintable(QStringLiteral("%1 %2").arg(fxIn).arg(fxOut)));
        buses.setEffectMetering({});
        QVERIFY(buses.takeEffectLevels().empty());

        // Bypassed: full level again, through the same node.
        devices.setEffectEnabled(devices.rowForKey(micKey), 0, false);
        QTest::qWait(100);
        const float bypassed = peakWhilePlaying(1200);
        QVERIFY2(bypassed > 0.3F, qPrintable(QString::number(bypassed)));
        QCOMPARE(nodeNamed(m_graph, micFx)->id, fxNode->id);
        buses.setMetering(false);

        // A mix bus: bus -> effects -> sends.
        const auto mixFx = effectsNodeName(mixNode);
        const auto mixSend =
            sendNodeName(store.find(mixKey.toStdString())->busId, store.find(sinkKey)->busId);
        devices.setSend(devices.rowForKey(mixKey.toStdString()), QString::fromStdString(sinkKey),
                        true);
        devices.addEffect(devices.rowForKey(mixKey.toStdString()), QStringLiteral("gain"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mixNode, mixFx) == 2 &&
                       linkCount(m_graph, mixFx, mixSend) == 2 &&
                       linkCount(m_graph, mixNode, mixSend) == 0;
            },
            5000));

        // A ducker on the mix keyed by the mic: the mic's output feeds the
        // mix's effects node's key input (both mic channels into one port).
        const auto mixRow = [&] { return devices.rowForKey(mixKey.toStdString()); };
        devices.addEffect(mixRow(), QStringLiteral("ducker"));
        devices.setEffectKey(mixRow(), 1, QString::fromStdString(micKey));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, inBus, mixFx) == 2 && // the key
                       linkCount(m_graph, mixNode, mixFx) == 2 &&
                       linkCount(m_graph, mixFx, mixSend) == 2;
            },
            5000));
        // The other way round would be a loop (the mic feeds the mix): refused.
        devices.addEffect(devices.rowForKey(micKey), QStringLiteral("ducker"));
        devices.setEffectKey(devices.rowForKey(micKey), 1, mixKey);
        QTest::qWait(1000);
        QCOMPARE(linkCount(m_graph, mixFx, micFx), 0);
        QCOMPARE(linkCount(m_graph, mixNode, micFx), 0);
        devices.removeEffect(devices.rowForKey(micKey), 1);
        // Unkeyed: the key link goes.
        devices.setEffectKey(mixRow(), 1, QString());
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, inBus, mixFx) == 0; }, 5000));

        // An output strip: bus -> effects -> hardware.
        const auto outFx = effectsNodeName(outBus);
        devices.addEffect(devices.rowForKey(sinkKey), QStringLiteral("gain"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, outBus, outFx) == 2 &&
                       linkCount(m_graph, outFx, m_sinkName) == 2 &&
                       linkCount(m_graph, outBus, m_sinkName) == 0;
            },
            5000));

        // No effects: the nodes go and the direct links come back.
        devices.removeEffect(devices.rowForKey(micKey), 0);
        devices.removeEffect(devices.rowForKey(sinkKey), 0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !nodeNamed(m_graph, micFx) && !nodeNamed(m_graph, outFx) &&
                       linkCount(m_graph, mic, inBus) == 2 &&
                       sentThrough(m_graph, inBus, mixNode, send, 2) &&
                       linkCount(m_graph, outBus, m_sinkName) == 2;
            },
            5000));

        devices.forget(devices.rowForKey(mixKey.toStdString()));
        devices.setEnabled(devices.rowForKey(micKey), false);
        devices.setEnabled(devices.rowForKey(sinkKey), false);
        QVERIFY(QTest::qWaitFor(
            [&] { return !nodeNamed(m_graph, inBus) && !nodeNamed(m_graph, outBus); }, 5000));
        m_busName.clear();
    }

    void cuesToHeadphones()
    {
        // The test sink as headphones; a fake mic cued to them.
        const auto pid = std::to_string(QCoreApplication::applicationPid());
        const auto feed = "pp_cuefeed_" + pid;
        const auto mic = "pp_cuesrc_" + pid;
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
        m_owners.insert(busOwnerFor(store.path()));
        const auto micKey = makeEndpointKey("Audio/Source", mic);
        const auto sinkKey = makeEndpointKey("Audio/Sink", m_sinkName);
        QVERIFY(QTest::qWaitFor(
            [&] { return devices.rowForKey(micKey) >= 0 && devices.rowForKey(sinkKey) >= 0; },
            5000));
        devices.setEnabled(devices.rowForKey(micKey), true);
        devices.setEnabled(devices.rowForKey(sinkKey), true);
        const auto inBus = store.find(micKey)->busName;
        const auto outBus = store.find(sinkKey)->busName;
        m_busName = outBus;
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, outBus, m_sinkName) == 2; }, 5000));

        devices.setCueOutput(sinkKey);
        devices.setCue(devices.rowForKey(micKey), true);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, inBus, m_sinkName) == 2; }, 5000));
        QCOMPARE(linkCount(m_graph, outBus, m_sinkName), 2); // its own path stays (muted)
        // A mix bus cued, then given effects: its cue link moves from the bus
        // to its effects node.
        const auto mixKey =
            devices.createVirtual(QStringLiteral("Cue Mix"), EndpointKind::VirtualSink);
        const auto mixNode = store.find(mixKey.toStdString())->busName;
        devices.setCue(devices.rowForKey(mixKey.toStdString()), true);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, mixNode, m_sinkName) == 2; }, 5000));
        devices.addEffect(devices.rowForKey(mixKey.toStdString()), QStringLiteral("gain"));
        const auto mixFx = effectsNodeName(mixNode);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return linkCount(m_graph, mixFx, m_sinkName) == 2 &&
                       linkCount(m_graph, mixNode, m_sinkName) == 0;
            },
            5000));
        devices.forget(devices.rowForKey(mixKey.toStdString()));

        // Two strips keyed from each other would be a loop: only one key link.
        devices.addEffect(devices.rowForKey(micKey), QStringLiteral("ducker"));
        devices.setEffectKey(devices.rowForKey(micKey), 0, QString::fromStdString(sinkKey));
        devices.addEffect(devices.rowForKey(sinkKey), QStringLiteral("ducker"));
        devices.setEffectKey(devices.rowForKey(sinkKey), 0, QString::fromStdString(micKey));
        const auto micFx = effectsNodeName(inBus);
        const auto outFx = effectsNodeName(outBus);
        QVERIFY(QTest::qWaitFor(
            [&] { return nodeNamed(m_graph, micFx) != nullptr && nodeNamed(m_graph, outFx) != nullptr; },
            5000));
        QTest::qWait(1000);
        const auto micKeyed = linkCount(m_graph, outFx, micFx);  // the output keys the mic
        const auto outKeyed = linkCount(m_graph, inBus, outFx);  // the mic keys the output
        QVERIFY2((micKeyed == 2) != (outKeyed == 2) && micKeyed + outKeyed == 2,
                 qPrintable(QStringLiteral("%1 %2").arg(micKeyed).arg(outKeyed)));
        devices.removeEffect(devices.rowForKey(micKey), 0);
        devices.removeEffect(devices.rowForKey(sinkKey), 0);

        // Uncued: the cue link goes, the output's own path stays.
        devices.setCue(devices.rowForKey(micKey), false);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, inBus, m_sinkName) == 0; }, 5000));
        QCOMPARE(linkCount(m_graph, outBus, m_sinkName), 2);
        // Cued again, then cue turned off: gone too.
        devices.setCue(devices.rowForKey(micKey), true);
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, inBus, m_sinkName) == 2; }, 5000));
        devices.setCueOutput({});
        QVERIFY(QTest::qWaitFor([&] { return linkCount(m_graph, inBus, m_sinkName) == 0; }, 5000));
        QCOMPARE(linkCount(m_graph, outBus, m_sinkName), 2);

        devices.setEnabled(devices.rowForKey(micKey), false);
        devices.setEnabled(devices.rowForKey(sinkKey), false);
        QVERIFY(QTest::qWaitFor(
            [&] { return !nodeNamed(m_graph, inBus) && !nodeNamed(m_graph, outBus); }, 5000));
        m_busName.clear();
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
        m_owners.insert(busOwnerFor(store.path()));

        const auto key = makeEndpointKey("Audio/Sink", name);
        QVERIFY(QTest::qWaitFor([&] { return devices.rowForKey(key) >= 0; }, 5000));
        devices.setChannelPair(devices.rowForKey(key), 2); // channels 3-4
        devices.setEnabled(devices.rowForKey(key), true);
        const auto bus = store.find(key)->busName;

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

        // An app that picks its own device (--target, like Audacity's device
        // setting) keeps it: no route is written over it, so it isn't linked
        // to the default as well, and its choice is remembered as its route.
        player->kill();
        player->waitForFinished();
        QVERIFY(QTest::qWaitFor([&] { return !streamId(); }, 5000));
        player->start(QStringLiteral("pw-play"),
                      {QStringLiteral("--volume=0"), QStringLiteral("--target"),
                       QString::fromStdString(m_sinkName), QStringLiteral("-P"),
                       QStringLiteral("{ application.name = \"Patch Prime Test\" "
                                      "application.process.binary = \"patchprime-test\" "
                                      "patchprime.scope = \"%1\" }")
                           .arg(QString::fromStdString(scope)),
                       tone});
        QVERIFY(QTest::qWaitFor([&] { return store.find(key)->target == m_sinkName; }, 5000));
        QTest::qWait(static_cast<int>(2 * AppRouter::settleTime.count())); // past the settling
        QVERIFY(m_graph.streamTarget(*streamId()).empty()); // nothing written over it
        const auto destinations = [&] {
            std::set<std::string> out;
            for (const auto &[id, link] : m_graph.links()) {
                if (prop(link, "link.output.node") == std::to_string(*streamId())) {
                    const auto in = m_graph.nodes().find(
                        static_cast<std::uint32_t>(std::stoul(prop(link, "link.input.node"))));
                    if (in != m_graph.nodes().end()) {
                        out.insert(prop(in->second, "node.name"));
                    }
                }
            }
            return out;
        };
        QCOMPARE(destinations(), std::set<std::string>{m_sinkName});
    }

private:
    PipeWireGraph m_graph;
    std::string m_sinkName;
    std::string m_busName;
    std::string m_originalDefault;
    std::string m_originalSource;
    std::set<std::string> m_owners;
    QString m_module;
};

QTEST_GUILESS_MAIN(TestBusLive)
#include "test_buslive.moc"

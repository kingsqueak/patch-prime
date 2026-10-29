// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// Live test against the running PipeWire server: a temporary null sink stands
// in for hardware. Enabling it must create its bus, link the bus monitor to it,
// apply the fader, meter a tone played into the bus, and remove the bus again.

#include "busmanager.h"
#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdlib>
#include <optional>

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
bool writeTone(const QString &path)
{
    constexpr int rate = 48000;
    constexpr int frames = rate * 3;
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
    for (int i = 0; i < frames; ++i) {
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

} // namespace

class TestBusLive : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        for (const auto *tool : {"pactl", "pw-dump", "pw-play"}) {
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

private:
    PipeWireGraph m_graph;
    std::string m_sinkName;
    std::string m_busName;
    std::string m_originalDefault;
    QString m_module;
};

QTEST_GUILESS_MAIN(TestBusLive)
#include "test_buslive.moc"

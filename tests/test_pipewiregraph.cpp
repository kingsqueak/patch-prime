// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// Live test against the running PipeWire server: creates a temporary null
// sink with pactl and checks that the graph and the device model follow it.

#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QCoreApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstdlib>
#include <optional>

using namespace pp;

namespace {

// Runs pactl; returns its trimmed output, or nullopt when it failed.
std::optional<QString> pactl(const QStringList &args)
{
    QProcess process;
    process.start(QStringLiteral("pactl"), args);
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return std::nullopt;
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

const GraphObject *findNode(const PipeWireGraph &graph, const std::string &name)
{
    for (const auto &[id, node] : graph.nodes()) {
        if (prop(node, "node.name") == name) {
            return &node;
        }
    }
    return nullptr;
}

} // namespace

class TestPipeWireGraph : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("pactl")).isEmpty()) {
            qWarning("pactl not found");
            std::exit(77); // ctest SKIP_RETURN_CODE
        }
        m_graph.start();
        if (!QTest::qWaitFor([this] { return m_graph.isConnected() && !m_graph.nodes().empty(); },
                             5000)) {
            qWarning("no PipeWire server");
            std::exit(77);
        }
        m_sinkName = "pp_test_sink_" + std::to_string(QCoreApplication::applicationPid());
    }

    void cleanupTestCase()
    {
        if (!m_module.isEmpty()) {
            pactl({QStringLiteral("unload-module"), m_module});
        }
    }

    void nodesHaveFullProps()
    {
        // object.serial is only in the info props, so this shows the bind worked.
        for (const auto &[id, node] : m_graph.nodes()) {
            QVERIFY2(node.props.contains("object.serial"), prop(node, "node.name").c_str());
        }
    }

    void followsHotplug()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel model(&store);
        model.attach(&m_graph);
        const auto key = makeEndpointKey("Audio/Sink", m_sinkName);

        m_module = pactl({QStringLiteral("load-module"), QStringLiteral("module-null-sink"),
                          QStringLiteral("sink_name=") + QString::fromStdString(m_sinkName)})
                       .value_or(QString());
        QVERIFY2(!m_module.isEmpty(), "pactl load-module failed");

        QVERIFY(QTest::qWaitFor([&] { return findNode(m_graph, m_sinkName) != nullptr; }, 5000));
        const auto *node = findNode(m_graph, m_sinkName);
        QCOMPARE(prop(*node, "media.class"), "Audio/Sink");

        QVERIFY(QTest::qWaitFor([&] { return model.rowForKey(key) >= 0; }, 2000));
        const int row = model.rowForKey(key);
        QCOMPARE(model.data(model.index(row), DeviceModel::VirtualRole).toBool(), true);
        QCOMPARE(model.data(model.index(row), DeviceModel::ChannelsRole).toInt(), 2);
        model.setEnabled(row, true);

        // Its ports show up too.
        const auto nodeId = std::to_string(node->id);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::ranges::any_of(m_graph.ports(), [&](const auto &entry) {
                    return prop(entry.second, "node.id") == nodeId;
                });
            },
            2000));

        QVERIFY(pactl({QStringLiteral("unload-module"), m_module}).has_value());
        m_module.clear();
        QVERIFY(QTest::qWaitFor([&] { return findNode(m_graph, m_sinkName) == nullptr; }, 5000));
        QVERIFY(std::ranges::none_of(m_graph.ports(), [&](const auto &entry) {
            return prop(entry.second, "node.id") == nodeId;
        }));

        // Saved, so it stays listed as offline.
        const int offline = model.rowForKey(key);
        QVERIFY(offline >= 0);
        QCOMPARE(model.data(model.index(offline), DeviceModel::OnlineRole).toBool(), false);
    }

    void stopReportsEverythingRemoved()
    {
        QSignalSpy removed(&m_graph, &PipeWireGraph::objectRemoved);
        QSignalSpy connected(&m_graph, &PipeWireGraph::connectedChanged);
        const auto before = m_graph.nodes().size() + m_graph.devices().size() +
                            m_graph.ports().size() + m_graph.links().size();
        m_graph.stop();
        QCOMPARE(static_cast<std::size_t>(removed.count()), before);
        QCOMPARE(connected.count(), 1);
        QVERIFY(m_graph.nodes().empty());
        QVERIFY(!m_graph.isConnected());
    }

private:
    PipeWireGraph m_graph;
    std::string m_sinkName;
    QString m_module;
};

QTEST_GUILESS_MAIN(TestPipeWireGraph)
#include "test_pipewiregraph.moc"

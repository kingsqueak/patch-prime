// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "graphsnapshot.h"

#include <QTest>

#include <algorithm>
#include <cstdlib>

class TestGraphSnapshot : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        const auto snapshot = pp::takeGraphSnapshot({.timeout = std::chrono::milliseconds(3000)});
        if (!snapshot) {
            qWarning("no PipeWire server: %s", snapshot.error().c_str());
            std::exit(77); // ctest SKIP_RETURN_CODE
        }
        m_snapshot = *snapshot;
    }

    void hasServerInfo()
    {
        QVERIFY(!m_snapshot.server.version.empty());
        QVERIFY(!m_snapshot.server.libraryVersion.empty());
    }

    void nodesHaveFullProps()
    {
        // There's always at least the driver nodes (Dummy-Driver, Freewheel-Driver).
        QVERIFY(!m_snapshot.nodes.empty());
        for (const auto &node : m_snapshot.nodes) {
            QVERIFY2(node.props.contains("node.name"), node.type.c_str());
            // object.serial is only in the full info props, so this checks the bind worked.
            QVERIFY2(node.props.contains("object.serial"), node.props.at("node.name").c_str());
        }
    }

    void portsReferToKnownNodes()
    {
        for (const auto &port : m_snapshot.ports) {
            const auto it = port.props.find("node.id");
            QVERIFY(it != port.props.end());
            const auto nodeId = static_cast<std::uint32_t>(std::stoul(it->second));
            const bool known = std::ranges::any_of(
                m_snapshot.nodes, [nodeId](const pp::GraphObject &n) { return n.id == nodeId; });
            QVERIFY2(known, it->second.c_str());
        }
    }

private:
    pp::GraphSnapshot m_snapshot;
};

QTEST_GUILESS_MAIN(TestGraphSnapshot)
#include "test_graphsnapshot.moc"

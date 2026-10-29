// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"

#include <QTest>

#include <cmath>

using namespace pp;

namespace {

PortRef port(std::uint32_t id, std::string channel)
{
    return {.nodeId = 1, .portId = id, .channel = std::move(channel)};
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
ids(const std::vector<std::pair<PortRef, PortRef>> &pairs)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.reserve(pairs.size());
    for (const auto &[o, i] : pairs) {
        out.emplace_back(o.portId, i.portId);
    }
    return out;
}

using Pairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

} // namespace

class TestBusManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void names()
    {
        QCOMPARE(busNodeName(Direction::Input, "ab12cd34"), "patchprime.bus.in.ab12cd34");
        QCOMPARE(busNodeName(Direction::Output, "ab12cd34"), "patchprime.bus.out.ab12cd34");
    }

    void decibels()
    {
        QCOMPARE(dbToLinear(0.0, -60.0), 1.0F);
        QVERIFY(std::fabs(dbToLinear(-6.0, -60.0) - 0.501187F) < 1e-5F);
        QVERIFY(std::fabs(dbToLinear(6.0, -60.0) - 1.995262F) < 1e-5F);
        QCOMPARE(dbToLinear(-60.0, -60.0), 0.0F);
        QCOMPARE(dbToLinear(-80.0, -60.0), 0.0F);
    }

    void positions()
    {
        Endpoint e;
        e.channels = 2;
        e.positions = {"FL", "FR"};
        QCOMPARE(busPositions(e), (std::vector<std::string>{"FL", "FR"}));
        e.positions = {"AUX0", "AUX1"};
        QCOMPARE(busPositions(e), (std::vector<std::string>{"AUX0", "AUX1"}));
        e.positions.clear();
        QCOMPARE(busPositions(e), (std::vector<std::string>{"FL", "FR"}));
        e.channels = 1;
        QCOMPARE(busPositions(e), (std::vector<std::string>{"MONO"}));
        e.channels = 0; // unknown: stereo
        QCOMPARE(busPositions(e), (std::vector<std::string>{"FL", "FR"}));
        e.channels = 4;
        QCOMPARE(busPositions(e), (std::vector<std::string>{"AUX0", "AUX1", "AUX2", "AUX3"}));
    }

    void properties()
    {
        Endpoint e;
        e.key = "Audio/Source|mic";
        e.direction = Direction::Input;
        e.description = "USB Mic";
        e.channels = 1;
        DeviceSettings s;
        s.busId = "0badf00d";
        auto props = busProperties(e, s);
        QCOMPARE(props["node.name"], "patchprime.bus.in.0badf00d");
        QCOMPARE(props["node.description"], "Patch Prime In: USB Mic");
        QCOMPARE(props["audio.position"], "[ MONO ]");
        QCOMPARE(props["node.autoconnect"], "false");
        QCOMPARE(props["object.linger"], "true");
        QCOMPARE(props["monitor.channel-volumes"], "true");
        QCOMPARE(props["patchprime.key"], "Audio/Source|mic");
        QCOMPARE(busProperties(e, s, "cafe1234")["patchprime.owner"], "cafe1234");
        QCOMPARE(busOwnerFor(QStringLiteral("/a/devices.json")).size(), std::size_t{8});
        QVERIFY(busOwnerFor(QStringLiteral("/a/devices.json")) !=
                busOwnerFor(QStringLiteral("/b/devices.json")));
        s.label = "Desk";
        QCOMPARE(busProperties(e, s)["node.description"], "Patch Prime In: Desk");
    }

    void pairing()
    {
        // Same channel names, in a different order: by name.
        QCOMPARE(ids(pairPorts({port(1, "FL"), port(2, "FR")}, {port(11, "FR"), port(10, "FL")})),
                 (Pairs{{1, 10}, {2, 11}}));
        // Mono to stereo: fan out.
        QCOMPARE(ids(pairPorts({port(1, "MONO")}, {port(10, "FL"), port(11, "FR")})),
                 (Pairs{{1, 10}, {1, 11}}));
        // Stereo to mono: mix down.
        QCOMPARE(ids(pairPorts({port(1, "FL"), port(2, "FR")}, {port(10, "MONO")})),
                 (Pairs{{1, 10}, {2, 10}}));
        // Different names (pro-audio AUX vs FL/FR): by order.
        QCOMPARE(
            ids(pairPorts({port(1, "AUX0"), port(2, "AUX1")}, {port(10, "FL"), port(11, "FR")})),
            (Pairs{{1, 10}, {2, 11}}));
        // Unequal counts: as many as fit.
        QCOMPARE(ids(pairPorts({port(1, "AUX0"), port(2, "AUX1"), port(3, "AUX2")},
                               {port(10, "FL"), port(11, "FR")})),
                 (Pairs{{1, 10}, {2, 11}}));
        QVERIFY(pairPorts({}, {port(10, "FL")}).empty());
        QVERIFY(pairPorts({port(1, "FL")}, {}).empty());
    }
};

QTEST_GUILESS_MAIN(TestBusManager)
#include "test_busmanager.moc"

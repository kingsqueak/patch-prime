// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "audiosetup.h"
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
        // Buses outrank any hardware as the graph driver (a chosen master
        // interface outranks them), but are never picked as default devices.
        QCOMPARE(props["priority.driver"], "20000");
        QCOMPARE(props["priority.session"], "0");
        QVERIFY(busDriverPriority < clockDevicePriority());
        QCOMPARE(busOwnerFor(QStringLiteral("/a/devices.json")).size(), std::size_t{8});
        QVERIFY(busOwnerFor(QStringLiteral("/a/devices.json")) !=
                busOwnerFor(QStringLiteral("/b/devices.json")));
        s.label = "Desk";
        QCOMPARE(busProperties(e, s)["node.description"], "Patch Prime In: Desk");
    }

    void kinds()
    {
        Endpoint mic;
        mic.key = "Audio/Source|mic";
        mic.direction = Direction::Input;
        mic.description = "Mic";
        DeviceSettings hw;
        hw.busId = "11111111";
        QCOMPARE(busMediaClass(mic, hw), "Audio/Source/Virtual"); // recordable, not an output
        QCOMPARE(busNameFor(mic, hw), "patchprime.bus.in.11111111");

        Endpoint mix;
        mix.direction = Direction::Output;
        DeviceSettings sink;
        sink.busId = "22222222";
        sink.kind = EndpointKind::VirtualSink;
        sink.label = "Stream Mix";
        QCOMPARE(busNameFor(mix, sink), "patchprime.virtual.22222222");
        QCOMPARE(busMediaClass(mix, sink), "Audio/Sink");
        QCOMPARE(busDescription(mix, sink), "Stream Mix");
        sink.kind = EndpointKind::VirtualSource;
        QCOMPARE(busMediaClass(mix, sink), "Audio/Source/Virtual");
        sink.kind = EndpointKind::VirtualInput; // played into, routed like an input
        QCOMPARE(busMediaClass(mix, sink), "Audio/Sink");
        QCOMPARE(busNameFor(mix, sink), "patchprime.virtual.22222222");
    }

    void readableNames()
    {
        QCOMPARE(nameSlug("Maudio 1"), "maudio-1");
        QCOMPARE(nameSlug("  PC Snd Crd!! "), "pc-snd-crd");
        QCOMPARE(nameSlug("Scarlett8i6"), "scarlett8i6");
        QCOMPARE(nameSlug("Café Mix"), "caf-mix"); // non-ASCII dropped
        QCOMPARE(nameSlug("***"), "device");

        DeviceStore store(QStringLiteral("/dev/null/unused"));
        auto &mic = store.upsert("Audio/Source|mic");
        mic.direction = Direction::Input;
        mic.label = "Mic1";
        auto &phones = store.upsert("Audio/Sink|phones");
        phones.direction = Direction::Output;
        phones.description = "Maudio 1"; // no label: the description
        auto &desktop = store.upsert(DeviceStore::virtualKey("3d5c6eea"));
        desktop.kind = EndpointKind::VirtualInput;
        desktop.label = "Desktop";
        auto &send = store.upsert(DeviceStore::virtualKey("11111111"));
        send.kind = EndpointKind::VirtualSink;
        send.label = "Send"; // a word Patch Prime uses itself
        QCOMPARE(readableBusName(store, "Audio/Source|mic"), "patchprime.in.mic1");
        QCOMPARE(readableBusName(store, "Audio/Sink|phones"), "patchprime.out.maudio-1");
        QCOMPARE(readableBusName(store, DeviceStore::virtualKey("3d5c6eea")), "patchprime.desktop");
        QCOMPARE(readableBusName(store, DeviceStore::virtualKey("11111111")), "patchprime.send-1");

        // A clash gets a number.
        store.upsert("Audio/Source|other").busName = "patchprime.in.mic1";
        QCOMPARE(readableBusName(store, "Audio/Source|mic"), "patchprime.in.mic1-2");
    }

    void migratesOldNames()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        auto &mic = store.upsert("Audio/Source|mic");
        mic.direction = Direction::Input;
        mic.label = "Mic1";
        const auto micId = mic.busId;
        auto &desktop = store.upsert(DeviceStore::virtualKey("3d5c6eea"));
        desktop.busId = "3d5c6eea";
        desktop.kind = EndpointKind::VirtualInput;
        desktop.label = "Desktop";
        desktop.nodeName = virtualNodeName("3d5c6eea");
        store.upsert("Audio/Sink|named").busName = "patchprime.out.keep";

        const auto renamed = migrateBusNames(store);
        QCOMPARE(renamed.size(), 2U); // the one already named is left alone
        QCOMPARE(renamed.at(busNodeName(Direction::Input, micId)), "patchprime.in.mic1");
        QCOMPARE(renamed.at("patchprime.virtual.3d5c6eea"), "patchprime.desktop");
        // A virtual device is its own bus: its node name follows.
        QCOMPARE(store.find(DeviceStore::virtualKey("3d5c6eea"))->nodeName, "patchprime.desktop");
        QCOMPARE(store.find("Audio/Sink|named")->busName, "patchprime.out.keep");
        QVERIFY(migrateBusNames(store).empty()); // once only
    }

    void creationLimiter()
    {
        using Decision = CreationLimiter::Decision;
        CreationLimiter limiter;
        const auto start = CreationLimiter::Clock::time_point{};
        for (int i = 0; i < CreationLimiter::maxPerWindow; ++i) {
            QCOMPARE(limiter.attempt("bus", start + std::chrono::milliseconds(50 * i)),
                     Decision::Create);
        }
        // Over the limit: refused, and flagged once for a warning.
        QCOMPARE(limiter.attempt("bus", start + std::chrono::seconds(1)), Decision::RefuseFirst);
        QCOMPARE(limiter.attempt("bus", start + std::chrono::seconds(2)), Decision::Refuse);
        QCOMPARE(limiter.attempt("other", start + std::chrono::seconds(2)), Decision::Create);
        // A minute after the first attempt, it may try again.
        QCOMPARE(limiter.attempt("bus", start + CreationLimiter::window), Decision::Create);
        // Once it appears, counting starts afresh.
        for (int i = 1; i < CreationLimiter::maxPerWindow; ++i) {
            QCOMPARE(limiter.attempt("bus", start + CreationLimiter::window), Decision::Create);
        }
        limiter.appeared("bus");
        QCOMPARE(limiter.attempt("bus", start + CreationLimiter::window), Decision::Create);
    }

    void channelPairs()
    {
        const std::vector<PortRef> six{port(1, "AUX0"), port(2, "AUX1"), port(3, "AUX2"),
                                       port(4, "AUX3"), port(5, "AUX4"), port(6, "AUX5")};
        auto idsOf = [](const std::vector<PortRef> &ports) {
            std::vector<std::uint32_t> out;
            out.reserve(ports.size());
            for (const auto &p : ports) {
                out.push_back(p.portId);
            }
            return out;
        };
        QCOMPARE(idsOf(selectPair(six, 0)).size(), 6U);
        QCOMPARE(idsOf(selectPair(six, 1)), (std::vector<std::uint32_t>{1, 2}));
        QCOMPARE(idsOf(selectPair(six, 3)), (std::vector<std::uint32_t>{5, 6}));
        QCOMPARE(idsOf(selectPair(six, 4)).size(), 6U); // no such pair: all
        QCOMPARE(idsOf(selectPair({port(1, "FL")}, 1)).size(), 1U);
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

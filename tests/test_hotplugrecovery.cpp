// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicestore.h"
#include "hotplugrecovery.h"

#include <QTest>

using namespace pp;

namespace {

const std::string input = "alsa_input.usb-Scarlett-00.pro-input-0";
const std::string output = "alsa_output.usb-Scarlett-00.pro-output-0";

GraphObject node(std::uint32_t id, const std::string &name, const std::string &profile)
{
    return {.id = id,
            .type = "PipeWire:Interface:Node",
            .version = 3,
            .props = {{"node.name", name}, {"device.profile.name", profile}}};
}

GraphObject card(std::uint32_t id, const std::string &name)
{
    return {.id = id,
            .type = "PipeWire:Interface:Device",
            .version = 3,
            .props = {{"device.name", name}}};
}

void addDevice(DeviceStore &store, const std::string &nodeName, bool enabled = true)
{
    auto &s = store.upsert("Audio|" + nodeName);
    s.nodeName = nodeName;
    s.enabled = enabled;
    s.signature.profile = "pro-audio";
}

} // namespace

class TestHotplugRecovery : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void findsAHalfAttachedCard()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        addDevice(store, input);
        addDevice(store, output);
        const std::map<std::uint32_t, GraphObject> devices{
            {5, card(5, "alsa_card.usb-Scarlett-00")}};

        // The input came back in pro-audio, the output didn't: reset the card.
        std::map<std::uint32_t, GraphObject> nodes{{10, node(10, input, "pro-audio")}};
        auto found = findHalfAttached(nodes, devices, store);
        QCOMPARE(found.size(), 1U);
        QCOMPARE(found[0].card, "alsa_card.usb-Scarlett-00");
        QCOMPARE(found[0].key, "Audio|" + output);
        QVERIFY(!found[0].noNodes);

        // Both there: nothing to do.
        nodes.emplace(11, node(11, output, "pro-audio"));
        QVERIFY(findHalfAttached(nodes, devices, store).empty());
    }

    void leavesOtherCasesAlone()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        addDevice(store, input);
        addDevice(store, output);
        const std::map<std::uint32_t, GraphObject> devices{
            {5, card(5, "alsa_card.usb-Scarlett-00")}};

        // Unplugged: no card, no nodes.
        QVERIFY(findHalfAttached({}, {}, store).empty());
        // The card is there with no nodes at all (taken whole by another
        // session, or switched off: the card's profile decides later).
        const auto whole = findHalfAttached({}, devices, store);
        QCOMPARE(whole.size(), 2U);
        QVERIFY(whole[0].noNodes && whole[1].noNodes);
        // The card switched to another profile on purpose.
        QVERIFY(findHalfAttached({{10, node(10, "alsa_input.usb-Scarlett-00.multichannel-input",
                                            "analog-surround-21")}},
                                 devices, store)
                    .empty());
        // A disabled device isn't waited for.
        DeviceStore disabled(QStringLiteral("/dev/null/unused"));
        addDevice(disabled, input);
        addDevice(disabled, output, false);
        QVERIFY(findHalfAttached({{10, node(10, input, "pro-audio")}}, devices, disabled).empty());
    }

    void readsTheCardsProfile()
    {
        // pactl --format=json list cards, trimmed.
        const QByteArray json = R"([
            {"name": "alsa_card.pci-0000_0b_00.3", "active_profile": "output:analog-stereo+input:analog-stereo"},
            {"name": "alsa_card.usb-Scarlett-00", "active_profile": "pro-audio"},
            {"name": "alsa_card.pci-0000_09_00.1", "active_profile": "off"}
        ])";
        QCOMPARE(activeCardProfile(json, "alsa_card.pci-0000_0b_00.3"),
                 "output:analog-stereo+input:analog-stereo");
        QCOMPARE(activeCardProfile(json, "alsa_card.usb-Scarlett-00"), "pro-audio");
        QCOMPARE(activeCardProfile(json, "alsa_card.pci-0000_09_00.1"), "off");
        QVERIFY(activeCardProfile(json, "alsa_card.gone").empty());
        QVERIFY(activeCardProfile("not json", "alsa_card.usb-Scarlett-00").empty());
    }
};

QTEST_GUILESS_MAIN(TestHotplugRecovery)
#include "test_hotplugrecovery.moc"

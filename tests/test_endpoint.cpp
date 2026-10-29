// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "endpoint.h"

#include <QTest>

using namespace pp;

namespace {

GraphObject node(Properties props)
{
    return {.id = 44, .type = "PipeWire:Interface:Node", .version = 3, .props = std::move(props)};
}

GraphObject fastTrack()
{
    return {.id = 55,
            .type = "PipeWire:Interface:Device",
            .version = 3,
            .props = {{"device.name", "alsa_card.usb-M-Audio_Fast_Track-00"},
                      {"device.serial", "M-Audio_Fast_Track"},
                      {"device.vendor.id", "0x0763"},
                      {"device.product.id", "0x2024"},
                      {"device.bus-path", "pci-0000:04:00.0-usb-0:3:1.0"}}};
}

Signature sig(std::string serial, std::string busPath, std::string profile = "analog-stereo")
{
    return {.deviceSerial = std::move(serial),
            .vendorId = "0x0763",
            .productId = "0x2024",
            .busPath = std::move(busPath),
            .profile = std::move(profile)};
}

} // namespace

class TestEndpoint : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hardwareSink()
    {
        const auto device = fastTrack();
        const auto e = classifyEndpoint(
            node({{"media.class", "Audio/Sink"},
                  {"node.name", "alsa_output.usb-M-Audio_Fast_Track-00.iec958-stereo"},
                  {"node.description", "M-Audio Fast Track MKII Digital Stereo (IEC958)"},
                  {"device.id", "55"},
                  {"device.profile.name", "iec958-stereo"},
                  {"audio.channels", "2"},
                  {"audio.position", "[ FL, FR ]"}}),
            &device);
        QVERIFY(e.has_value());
        QCOMPARE(e->direction, Direction::Output);
        QCOMPARE(e->key, "Audio/Sink|alsa_output.usb-M-Audio_Fast_Track-00.iec958-stereo");
        QVERIFY(!e->isVirtual);
        QCOMPARE(e->channels, 2);
        QCOMPARE(e->positions, (std::vector<std::string>{"FL", "FR"}));
        QCOMPARE(e->deviceName, "alsa_card.usb-M-Audio_Fast_Track-00");
        QCOMPARE(e->signature.deviceSerial, "M-Audio_Fast_Track");
        QCOMPARE(e->signature.profile, "iec958-stereo");
    }

    void virtualSourceAndChannelFallback()
    {
        const auto e = classifyEndpoint(node({{"media.class", "Audio/Source/Virtual"},
                                              {"node.name", "mic-loopback"},
                                              {"audio.position", "[ MONO ]"}}),
                                        nullptr);
        QVERIFY(e.has_value());
        QCOMPARE(e->direction, Direction::Input);
        QVERIFY(e->isVirtual);
        QCOMPARE(e->channels, 1);
        QCOMPARE(e->description, "mic-loopback"); // falls back to node.name
    }

    void rejectsNonEndpoints_data()
    {
        QTest::addColumn<QString>("mediaClass");
        QTest::addColumn<QString>("nodeName");
        QTest::newRow("stream") << "Stream/Output/Audio" << "firefox";
        QTest::newRow("midi") << "Midi/Bridge" << "Midi-Bridge";
        QTest::newRow("driver") << "" << "Dummy-Driver";
        QTest::newRow("duplex") << "Audio/Duplex" << "pro-audio";
        QTest::newRow("our own bus") << "Audio/Sink" << "patchprime.bus.out.1";
        QTest::newRow("no name") << "Audio/Sink" << "";
    }

    void rejectsNonEndpoints()
    {
        QFETCH(QString, mediaClass);
        QFETCH(QString, nodeName);
        QVERIFY(!classifyEndpoint(node({{"media.class", mediaClass.toStdString()},
                                        {"node.name", nodeName.toStdString()}}),
                                  nullptr));
    }

    void parsesPositions()
    {
        QCOMPARE(parsePositions("[ FL, FR, FC, LFE ]"),
                 (std::vector<std::string>{"FL", "FR", "FC", "LFE"}));
        QCOMPARE(parsePositions("FL,FR"), (std::vector<std::string>{"FL", "FR"}));
        QVERIFY(parsePositions("").empty());
        QVERIFY(parsePositions("[ ]").empty());
    }

    void matchesSignatures()
    {
        const auto saved = sig("ABC123", "usb-0:3");
        QCOMPARE(matchSignature(saved, sig("ABC123", "usb-0:5")), Match::SameSerial);
        QCOMPARE(matchSignature(sig("", "usb-0:3"), sig("", "usb-0:3", "iec958-stereo")),
                 Match::SameBusPath);
        QCOMPARE(matchSignature(sig("", "usb-0:3"), sig("", "usb-0:5")), Match::SameModel);
        // Different serials: two units of the same model.
        QCOMPARE(matchSignature(saved, sig("XYZ999", "usb-0:5")), Match::None);

        auto otherModel = sig("ABC123", "usb-0:3");
        otherModel.productId = "0x9999";
        QCOMPARE(matchSignature(saved, otherModel), Match::None);
        // Nothing to go on (virtual endpoints): never a match.
        QCOMPARE(matchSignature(Signature{}, Signature{}), Match::None);
    }
};

QTEST_GUILESS_MAIN(TestEndpoint)
#include "test_endpoint.moc"

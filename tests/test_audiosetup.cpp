// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "audiosetup.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "pipewiregraph.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace pp;

class TestAudioSetup : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void mixerSettings()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        auto wanted = wantedSettings(store); // the defaults: 48 kHz, PipeWire's block size
        QCOMPARE(wanted["clock.rate"], "48000");
        QCOMPARE(wanted["clock.allowed-rates"], "[ 48000 ]");
        QCOMPARE(wanted["clock.force-quantum"], "0");
        QVERIFY(!wanted.contains("clock.max-quantum")); // left to PipeWire
        QCOMPARE(followerPeriods(store), 8);              // two of its largest (2048) blocks
        store.setSampleRate(96000);
        store.setBlockSize(256);
        wanted = wantedSettings(store);
        QCOMPARE(wanted["clock.rate"], "96000");
        QCOMPARE(wanted["clock.allowed-rates"], "[ 96000 ]");
        QCOMPARE(wanted["clock.force-quantum"], "256");
        QCOMPARE(followerPeriods(store), 4); // a small fixed block: the minimum
        store.setBlockSize(2048);            // a big fixed block needs a bigger buffer
        QCOMPARE(followerPeriods(store), 8);
    }

    void deviceRules()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        auto &scarlett = store.upsert("Audio/Source|alsa_input.usb-Scarlett.pro-input-0");
        scarlett.nodeName = "alsa_input.usb-Scarlett.pro-input-0";
        auto &onboard = store.upsert("Audio/Sink|alsa_output.pci.pro-output-0");
        onboard.nodeName = "alsa_output.pci.pro-output-0";
        onboard.scheduling = Scheduling::Interrupt;
        onboard.sampleRate = 44100;
        store.upsert("Audio/Sink|bluez_output.x").nodeName = "bluez_output.x"; // not ALSA
        auto &mix = store.upsert(DeviceStore::virtualKey("12345678"));
        mix.kind = EndpointKind::VirtualSink;
        mix.nodeName = "patchprime.virtual.12345678";

        // Patch Prime's clock: automatic means timer scheduling.
        auto rules = wirePlumberRules(store);
        QVERIFY(rules.starts_with("# Written by Patch Prime"));
        QVERIFY(rules.contains("monitor.alsa.rules = ["));
        QVERIFY(rules.contains("node.name = \"alsa_input.usb-Scarlett.pro-input-0\" } ]\n"
                               "    actions = { update-props = { api.alsa.disable-tsched = false, api.alsa.period-num = 8 } }"));
        QVERIFY(rules.contains("node.name = \"alsa_output.pci.pro-output-0\" } ]\n"
                               "    actions = { update-props = { api.alsa.disable-tsched = true, "
                               "audio.rate = 44100 } }"));
        QVERIFY(!rules.contains("bluez"));
        QVERIFY(!rules.contains("patchprime.virtual"));

        // The hardware clock with no master chosen: Patch Prime's buses still
        // clock the graph, so everything follows on a timer.
        store.setClock(ClockMode::Hardware);
        rules = wirePlumberRules(store);
        QVERIFY(rules.contains("node.name = \"alsa_input.usb-Scarlett.pro-input-0\" } ]\n"
                               "    actions = { update-props = { api.alsa.disable-tsched = false, api.alsa.period-num = 8 } }"));
    }

    void clockDevice()
    {
        QCOMPARE(alsaCardOf("alsa_input.usb-Focusrite_8i6_USB_F8T-00.pro-input-0"),
                 "usb-Focusrite_8i6_USB_F8T-00");
        QCOMPARE(alsaCardOf("alsa_output.usb-Focusrite_8i6_USB_F8T-00.pro-output-0"),
                 "usb-Focusrite_8i6_USB_F8T-00");
        QVERIFY(alsaCardOf("patchprime.bus.in.1").empty());

        DeviceStore store(QStringLiteral("/dev/null/unused"));
        const std::string scarlettIn = "Audio/Source|alsa_input.usb-Scarlett-00.pro-input-0";
        store.upsert(scarlettIn).nodeName = "alsa_input.usb-Scarlett-00.pro-input-0";
        store.upsert("Audio/Sink|alsa_output.usb-Scarlett-00.pro-output-0").nodeName =
            "alsa_output.usb-Scarlett-00.pro-output-0";
        store.upsert("Audio/Source|alsa_input.usb-FastTrack-00.pro-input-0").nodeName =
            "alsa_input.usb-FastTrack-00.pro-input-0";
        store.setClock(ClockMode::Hardware);
        store.setClockDevice(scarlettIn);

        // The Scarlett input is the clock: top priority, and it keeps
        // interrupts; everything else follows on a timer, its own output too.
        const auto rules = wirePlumberRules(store);
        QVERIFY(rules.contains("\"alsa_input.usb-Scarlett-00.pro-input-0\" } ]\n"
                               "    actions = { update-props = { priority.driver = 30000 } }"));
        QVERIFY(rules.contains("\"alsa_output.usb-Scarlett-00.pro-output-0\" } ]\n"
                               "    actions = { update-props = { api.alsa.disable-tsched = false, api.alsa.period-num = 8 } }"));
        QVERIFY(rules.contains("\"alsa_input.usb-FastTrack-00.pro-input-0\" } ]\n"
                               "    actions = { update-props = { api.alsa.disable-tsched = false, api.alsa.period-num = 8 } }"));

        // The choice follows the device when it's recognised under a new name.
        QVERIFY(store.rekey(scarlettIn, "Audio/Source|alsa_input.usb-Scarlett-00.multichannel-input"));
        QCOMPARE(store.clockDevice(), "Audio/Source|alsa_input.usb-Scarlett-00.multichannel-input");
    }

    void rulesFileOnlyChangesWithTheSettings()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        PipeWireGraph graph; // never started: only the rule file is tested
        const auto path = dir.filePath(QStringLiteral("conf.d/60-patchprime.conf"));
        AudioSetup setup(&graph, &devices, path, false);
        QVERIFY(QFile::exists(path)); // written at start
        QVERIFY(!setup.writeRules()); // nothing changed

        store.upsert("Audio/Sink|alsa_output.x").nodeName = "alsa_output.x";
        QVERIFY(setup.writeRules());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("alsa_output.x"));
        QVERIFY(!setup.writeRules());
    }
};

QTEST_GUILESS_MAIN(TestAudioSetup)
#include "test_audiosetup.moc"

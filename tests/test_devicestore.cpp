// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicestore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace pp;

class TestDeviceStore : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void missingFileIsEmpty()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("nope/devices.json")));
        QVERIFY(store.load());
        QVERIFY(store.entries().empty());
    }

    void roundTrip()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("sub/devices.json"));
        {
            DeviceStore store(path);
            auto &d = store.upsert("Audio/Source|mic");
            d.enabled = true;
            d.label = "Desk mic";
            d.volumeDb = -12.5;
            d.muted = true;
            d.positions = {"MONO"};
            d.nodeName = "mic";
            d.mediaClass = "Audio/Source";
            d.direction = Direction::Input;
            d.description = "USB Microphone";
            d.channels = 1;
            d.signature = {.deviceSerial = "SN1",
                           .vendorId = "0x1",
                           .productId = "0x2",
                           .busPath = "usb-0:3",
                           .profile = "mono"};
            d.lastSeen = QDateTime(QDate(2026, 10, 29), QTime(13, 0), QTimeZone::UTC);
            QVERIFY2(store.save(), qPrintable(store.lastError())); // creates sub/
        }
        DeviceStore store(path);
        QVERIFY(store.load());
        const auto *d = store.find("Audio/Source|mic");
        QVERIFY(d != nullptr);
        QVERIFY(d->enabled);
        QCOMPARE(d->label, "Desk mic");
        QCOMPARE(d->volumeDb, -12.5);
        QVERIFY(d->muted);
        QCOMPARE(d->positions, std::vector<std::string>{"MONO"});
        QCOMPARE(d->busId.size(), std::size_t{8}); // made by upsert, kept by save/load
        QCOMPARE(d->direction, Direction::Input);
        QCOMPARE(d->channels, 1);
        QCOMPARE(d->signature.deviceSerial, "SN1");
        QCOMPARE(d->signature.busPath, "usb-0:3");
        QCOMPARE(d->lastSeen, QDateTime(QDate(2026, 10, 29), QTime(13, 0), QTimeZone::UTC));
    }

    void corruptFileIsAnError()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("devices.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{ not json");
        file.close();
        DeviceStore store(path);
        QVERIFY(!store.load());
        QVERIFY(!store.lastError().isEmpty());
    }

    void sendsAndKinds()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("devices.json"));
        {
            DeviceStore store(path);
            store.upsert("mic").sends = {"phones", "mix"};
            store.upsert("mic").sendLevels = {{"mix", -6.0}};
            store.upsert("mic").mono = true;
            store.upsert("mix").kind = EndpointKind::VirtualSink;
            store.upsert("chat").kind = EndpointKind::VirtualSource;
            store.upsert("phones").channelPair = 2;
            QCOMPARE(store.clock(), ClockMode::Internal); // the default
            store.setClock(ClockMode::Hardware);
            store.setClockDevice("phones");
            QCOMPARE(store.sampleRate(), 48000); // the default
            store.setSampleRate(96000);
            store.setBlockSize(512);
            store.upsert("phones").sampleRate = 44100;
            store.upsert("phones").scheduling = Scheduling::Timer;
            store.setCurrentScene("Show");
            store.setRecordingFormat("mp3");
            store.setPlayerTarget("mic");
            QVERIFY(store.save());
        }
        DeviceStore store(path);
        QVERIFY(store.load());
        QCOMPARE(store.find("mic")->sends, (std::vector<std::string>{"phones", "mix"}));
        QCOMPARE(store.find("mic")->sendLevels.at("mix"), -6.0);
        QVERIFY(store.find("mic")->mono);
        QCOMPARE(store.find("mix")->kind, EndpointKind::VirtualSink);
        QCOMPARE(store.find("chat")->kind, EndpointKind::VirtualSource);
        QCOMPARE(store.find("phones")->kind, EndpointKind::Hardware);
        QCOMPARE(store.find("phones")->channelPair, 2);
        QCOMPARE(store.clock(), ClockMode::Hardware);
        QCOMPARE(store.clockDevice(), "phones");
        QCOMPARE(store.sampleRate(), 96000);
        QCOMPARE(store.blockSize(), 512);
        QCOMPARE(store.find("phones")->sampleRate, 44100);
        QCOMPARE(store.find("phones")->scheduling, Scheduling::Timer);
        QCOMPARE(store.find("mic")->scheduling, Scheduling::Auto);
        QCOMPARE(store.find("mic")->channelPair, 0);
        QCOMPARE(store.currentScene(), "Show");
        QCOMPARE(store.recordingFormat(), "mp3");
        QCOMPARE(store.playerTarget(), "mic");

        // A re-attached destination keeps its sends; a removed one drops them.
        QVERIFY(store.rekey("phones", "phones2"));
        QCOMPARE(store.find("mic")->sends, (std::vector<std::string>{"phones2", "mix"}));
        QVERIFY(store.rekey("mix", "mix2")); // the send level follows its destination
        QCOMPARE(store.find("mic")->sendLevels.at("mix2"), -6.0);
        QVERIFY(store.remove("mix2"));
        QCOMPARE(store.find("mic")->sends, std::vector<std::string>{"phones2"});
        QVERIFY(store.find("mic")->sendLevels.empty());
        QCOMPARE(DeviceStore::virtualKey("ab12cd34"), "patchprime-virtual|ab12cd34");
    }

    void busIdSurvivesRekey()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        const auto id = store.upsert("a").busId;
        QVERIFY(!id.empty());
        QVERIFY(store.upsert("b").busId != id);
        QVERIFY(store.rekey("a", "c"));
        QCOMPARE(store.find("c")->busId, id);
    }

    void rekey()
    {
        DeviceStore store(QStringLiteral("/dev/null/unused"));
        store.upsert("a").label = "A";
        store.upsert("b").label = "B";
        QVERIFY(store.rekey("a", "c"));
        QVERIFY(store.find("a") == nullptr);
        QCOMPARE(store.find("c")->label, "A");
        QVERIFY(!store.rekey("c", "b")); // target taken: nothing moves
        QCOMPARE(store.find("c")->label, "A");
        QCOMPARE(store.find("b")->label, "B");
        QVERIFY(!store.rekey("missing", "d"));
    }
};

QTEST_GUILESS_MAIN(TestDeviceStore)
#include "test_devicestore.moc"

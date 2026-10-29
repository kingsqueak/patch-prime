// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "approuter.h"
#include "appstore.h"

#include <QTemporaryDir>
#include <QTest>

using namespace pp;

namespace {

GraphObject node(Properties props)
{
    return {.id = 50, .type = "PipeWire:Interface:Node", .version = 3, .props = std::move(props)};
}

} // namespace

class TestAppRouter : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void classifiesStreams()
    {
        const auto brave = classifyAppStream(node({{"media.class", "Stream/Output/Audio"},
                                                   {"node.name", "Brave"},
                                                   {"application.name", "Brave"},
                                                   {"application.process.binary", "brave"},
                                                   {"application.icon-name", "brave-browser"}}));
        QVERIFY(brave);
        QCOMPARE(brave->key, "playback|brave");
        QCOMPARE(brave->name, "Brave");
        QCOMPARE(brave->icon, "brave-browser");
        QCOMPARE(brave->direction, StreamDirection::Playback);

        // Recording; no binary or icon: the name stands in.
        const auto obs = classifyAppStream(node({{"media.class", "Stream/Input/Audio"},
                                                 {"node.name", "obs-capture"},
                                                 {"application.name", "OBS"}}));
        QVERIFY(obs);
        QCOMPARE(obs->key, "record|OBS");
        QCOMPARE(obs->direction, StreamDirection::Record);
        QVERIFY(obs->icon.empty());

        // Not app streams.
        QVERIFY(!classifyAppStream(node({{"media.class", "Audio/Sink"}, {"node.name", "x"}})));
        QVERIFY(!classifyAppStream(node({{"media.class", "Stream/Input/Audio"},
                                         {"node.name", "patchprime.meter.patchprime.bus.in.1"}})));
        QVERIFY(!classifyAppStream(node({{"media.class", "Stream/Input/Audio"},
                                         {"node.name", "peak"},
                                         {"node.passive", "true"}})));
        QVERIFY(!classifyAppStream(node({{"media.class", "Stream/Output/Audio"},
                                         {"node.name", "output.loopback"},
                                         {"node.link-group", "loopback-1"}})));
        auto device = node({{"media.class", "Stream/Output/Audio"}, {"node.name", "x"}});
        device.type = "PipeWire:Interface:Device";
        QVERIFY(!classifyAppStream(device));
    }

    void storeRoundTrip()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("sub/apps.json"));
        {
            AppStore store(path);
            QVERIFY(store.load()); // missing file: empty
            auto &a = store.upsert("record|obs");
            a.direction = StreamDirection::Record;
            a.name = "OBS";
            a.icon = "com.obsproject.Studio";
            a.target = "patchprime.virtual.12345678";
            a.lastSeen = QDateTime(QDate(2026, 10, 29), QTime(12, 0), QTimeZone::UTC);
            store.upsert("playback|brave").name = "Brave";
            QVERIFY(store.save());
        }
        AppStore store(path);
        QVERIFY(store.load());
        QCOMPARE(store.entries().size(), 2U);
        const auto *obs = store.find("record|obs");
        QVERIFY(obs != nullptr);
        QCOMPARE(obs->direction, StreamDirection::Record);
        QCOMPARE(obs->name, "OBS");
        QCOMPARE(obs->icon, "com.obsproject.Studio");
        QCOMPARE(obs->target, "patchprime.virtual.12345678");
        QCOMPARE(obs->lastSeen, QDateTime(QDate(2026, 10, 29), QTime(12, 0), QTimeZone::UTC));
        QVERIFY(store.find("playback|brave")->target.empty()); // the system default
        QVERIFY(store.remove("playback|brave"));
        QVERIFY(!store.remove("playback|brave"));
    }
};

QTEST_GUILESS_MAIN(TestAppRouter)
#include "test_approuter.moc"

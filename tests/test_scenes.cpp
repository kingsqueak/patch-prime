// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "appstore.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "scenes.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace pp;

namespace {

GraphObject node(std::uint32_t id, const std::string &mediaClass, const std::string &name)
{
    return {.id = id,
            .type = "PipeWire:Interface:Node",
            .version = 3,
            .props = {{"media.class", mediaClass},
                      {"node.name", name},
                      {"node.description", name},
                      {"audio.channels", "2"},
                      {"audio.position", "[ FL, FR ]"}}};
}

} // namespace

class TestScenes : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void mixRoundTrip()
    {
        DeviceSettings s;
        s.volumeDb = -6.5;
        s.muted = true;
        s.mono = true;
        s.sends = {"a", "b"};
        s.sendLevels = {{"a", -3.0}};
        s.label = "not part of the mix";
        DeviceSettings t;
        mixFromJson(t, mixToJson(s));
        QCOMPARE(t.volumeDb, -6.5);
        QVERIFY(t.muted && t.mono);
        QCOMPARE(t.sends, (std::vector<std::string>{"a", "b"}));
        QCOMPARE(t.sendLevels.at("a"), -3.0);
        QVERIFY(t.label.empty());
        // Fields a scene leaves out stay as they are.
        mixFromJson(t, QJsonObject{{QStringLiteral("muted"), false}});
        QVERIFY(!t.muted);
        QCOMPARE(t.volumeDb, -6.5);
    }

    void captureAndApply()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel model(&store);
        model.upsertNode(node(11, "Audio/Source", "mic"), nullptr);
        model.upsertNode(node(10, "Audio/Sink", "phones"), nullptr);
        const auto mic = model.rowForKey("Audio/Source|mic");
        model.setEnabled(mic, true);
        model.setEnabled(model.rowForKey("Audio/Sink|phones"), true);
        model.setVolume(model.rowForKey("Audio/Source|mic"), -10.0);
        model.setMono(model.rowForKey("Audio/Source|mic"), true);
        model.setSend(model.rowForKey("Audio/Source|mic"), QStringLiteral("Audio/Sink|phones"),
                      true);
        model.setSendLevel(model.rowForKey("Audio/Source|mic"), QStringLiteral("Audio/Sink|phones"),
                           -6.0);
        AppStore apps(dir.filePath(QStringLiteral("apps.json")));
        apps.upsert("playback|brave").target = "patchprime.desktop";

        const auto scene =
            captureScene(QStringLiteral("Streaming"), store, apps, "patchprime.desktop", "");
        QCOMPARE(scene.value(QLatin1String("version")).toInt(), sceneVersion);
        QCOMPARE(scene.value(QLatin1String("apps"))
                     .toObject()
                     .value(QLatin1String("playback|brave"))
                     .toString(),
                 QStringLiteral("patchprime.desktop"));

        // Change the mix, then load the scene: it comes back.
        model.setVolume(model.rowForKey("Audio/Source|mic"), 0.0);
        model.setMono(model.rowForKey("Audio/Source|mic"), false);
        model.setSend(model.rowForKey("Audio/Source|mic"), QStringLiteral("Audio/Sink|phones"),
                      false);
        QCOMPARE(applyScene(scene, model, nullptr, nullptr), 2);
        const auto *s = store.find("Audio/Source|mic");
        QCOMPARE(s->volumeDb, -10.0);
        QVERIFY(s->mono);
        QCOMPARE(s->sends, std::vector<std::string>{"Audio/Sink|phones"});
        QCOMPARE(s->sendLevels.at("Audio/Sink|phones"), -6.0);
    }

    void skipsWhatNoLongerApplies()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel model(&store);
        model.upsertNode(node(11, "Audio/Source", "mic"), nullptr);
        model.upsertNode(node(10, "Audio/Sink", "phones"), nullptr);
        QJsonObject strips{
            {QStringLiteral("Audio/Source|mic"),
             QJsonObject{
                 {QStringLiteral("volumeDb"), 99.0}, // clamped
                 {QStringLiteral("sends"), QJsonArray{QStringLiteral("Audio/Sink|gone"),
                                                      QStringLiteral("Audio/Sink|phones")}}}},
            {QStringLiteral("Audio/Sink|phones"),
             QJsonObject{{QStringLiteral("mono"), true}, // outputs have no mono
                         {QStringLiteral("sends"),
                          QJsonArray{QStringLiteral("Audio/Source|mic")}}}}, // backwards
            {QStringLiteral("Audio/Sink|gone"), QJsonObject{{QStringLiteral("muted"), true}}},
        };
        const QJsonObject scene{{QStringLiteral("strips"), strips}};
        QCOMPARE(applyScene(scene, model, nullptr, nullptr), 2); // "gone" has no strip
        QCOMPARE(store.find("Audio/Source|mic")->volumeDb, DeviceModel::maximumVolumeDb());
        QCOMPARE(store.find("Audio/Source|mic")->sends,
                 std::vector<std::string>{"Audio/Sink|phones"});
        QVERIFY(!store.find("Audio/Sink|phones")->mono);
        QVERIFY(store.find("Audio/Sink|phones")->sends.empty());
        QVERIFY(store.find("Audio/Sink|gone") == nullptr);
    }

    void storesScenes()
    {
        QTemporaryDir dir;
        SceneStore scenes(dir.filePath(QStringLiteral("scenes")));
        QVERIFY(scenes.names().isEmpty());
        QVERIFY(scenes.save(QJsonObject{{QStringLiteral("name"), QStringLiteral("Streaming")},
                                        {QStringLiteral("x"), 1}}));
        QVERIFY(scenes.save(QJsonObject{{QStringLiteral("name"), QStringLiteral(" Recording ")}}));
        QVERIFY(!scenes.save(QJsonObject{{QStringLiteral("name"), QStringLiteral("  ")}}));
        QCOMPARE(scenes.names(),
                 (QStringList{QStringLiteral("Recording"), QStringLiteral("Streaming")}));
        // Same name: replaced, not added.
        QVERIFY(scenes.save(QJsonObject{{QStringLiteral("name"), QStringLiteral("Streaming")},
                                        {QStringLiteral("x"), 2}}));
        QCOMPARE(scenes.names().size(), 2);
        QCOMPARE(scenes.load(QStringLiteral("Streaming")).value(QLatin1String("x")).toInt(), 2);
        // Stable across instances (a new process finds the same file).
        SceneStore again(dir.filePath(QStringLiteral("scenes")));
        QCOMPARE(again.load(QStringLiteral("Recording")).value(QLatin1String("name")).toString(),
                 QStringLiteral("Recording"));
        QVERIFY(again.remove(QStringLiteral("Recording")));
        QCOMPARE(scenes.names(), QStringList{QStringLiteral("Streaming")});
        QVERIFY(scenes.load(QStringLiteral("Recording")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestScenes)
#include "test_scenes.moc"

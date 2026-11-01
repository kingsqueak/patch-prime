// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"
#include "effects.h"
#include "presets.h"

#include <QTemporaryDir>
#include <QTest>

using namespace pp;

namespace {

EffectSettings effect(const std::string &type, std::map<std::string, double> params, bool enabled = true)
{
    EffectSettings e;
    e.type = type;
    e.enabled = enabled;
    e.params = std::move(params);
    return e;
}

GraphObject source(std::uint32_t id, const std::string &name)
{
    return {.id = id,
            .type = "PipeWire:Interface:Node",
            .version = 3,
            .props = {{"media.class", "Audio/Source"},
                      {"node.name", name},
                      {"node.description", name},
                      {"audio.channels", "2"},
                      {"audio.position", "[ FL, FR ]"}}};
}

} // namespace

class TestPresets : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void storeRoundTrip()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("sub/presets.json"));
        auto plugin = effect("lv2:urn:test", {{"gain", 3.0}});
        plugin.state = "<state/>";
        {
            PresetStore store(path);
            QVERIFY(store.load()); // missing: empty
            QVERIFY(store.saveChain(QStringLiteral(" Podcast mic "), {effect("gain", {{"gainDb", 6.0}}), plugin}));
            QVERIFY(!store.saveChain(QStringLiteral("  "), {}));
            auto ducker = effect("gain", {{"gainDb", -3.0}}, false);
            ducker.key = "some|strip";
            QVERIFY(store.saveEffect(QStringLiteral("Quiet"), ducker));
            QVERIFY(store.saveEffect(QStringLiteral("Loud"), effect("gain", {{"gainDb", 9.0}})));
            QVERIFY(store.saveEffect(QStringLiteral("Mine"), plugin));
        }
        PresetStore store(path);
        QVERIFY(store.load());
        QCOMPARE(store.chainNames(), QStringList{QStringLiteral("Podcast mic")});
        const auto chain = store.chain(QStringLiteral("Podcast mic"));
        QVERIFY(chain && chain->size() == 2);
        QCOMPARE(chain->at(0).params.at("gainDb"), 6.0);
        QCOMPARE(chain->at(1).state, std::string("<state/>")); // a plugin's own state comes along

        const auto names = store.effectNames();
        QCOMPARE(names.at("gain"), (QStringList{QStringLiteral("Loud"), QStringLiteral("Quiet")}));
        QCOMPARE(names.at("lv2:urn:test"), QStringList{QStringLiteral("Mine")});
        const auto quiet = store.effect("gain", QStringLiteral("Quiet"));
        QVERIFY(quiet);
        QVERIFY(quiet->enabled);     // settings, not the bypass
        QVERIFY(quiet->key.empty()); // nor the strip it listened to
        QVERIFY(!store.effect("eq", QStringLiteral("Quiet")));

        QVERIFY(store.removeEffect("gain", QStringLiteral("Loud")));
        QVERIFY(!store.removeEffect("gain", QStringLiteral("Loud")));
        QVERIFY(store.removeChain(QStringLiteral("Podcast mic")));
        PresetStore again(path);
        QVERIFY(again.load());
        QVERIFY(again.chainNames().isEmpty());
        QCOMPARE(again.effectNames().at("gain"), QStringList{QStringLiteral("Quiet")});
    }

    void loadsOntoStrips()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel devices(&store);
        devices.upsertNode(source(10, "mic"), nullptr);
        devices.setEnabled(0, true);
        const auto key = devices.rows().at(0).endpoint.key;
        devices.addEffect(0, QStringLiteral("gate"));
        devices.addEffect(0, QStringLiteral("gain"));
        // MIDI controls on both effects.
        MidiMapping gate{.id = "m1", .kind = MidiKind::Note, .channel = 0, .number = 1,
                         .target = {.action = MidiAction::EffectEnabled, .key = key, .effect = 0}};
        MidiMapping gain{.id = "m2", .kind = MidiKind::Cc, .channel = 0, .number = 7,
                         .target = {.action = MidiAction::EffectParam, .key = key, .effect = 1, .param = "gainDb"}};
        devices.setMidiMappings({gate, gain});

        // A chain where the gain stays second and the gate gives way to an EQ.
        devices.setEffects(0, {effect("eq", {}), effect("gain", {{"gainDb", 4.0}})});
        const auto &effects = store.find(key)->effects;
        QCOMPARE(effects.size(), 2U);
        QCOMPARE(effects.at(0).type, std::string("eq"));
        QCOMPARE(effects.at(1).params.at("gainDb"), 4.0);
        QCOMPARE(store.midiMappings().size(), 1U); // the gate's went
        QCOMPARE(store.midiMappings().front().id, std::string("m2"));

        // An effect preset: its settings, the effect's own bypass kept.
        devices.setEffectEnabled(0, 1, false);
        devices.loadEffectPreset(0, 1, effect("gain", {{"gainDb", -9.0}}));
        QCOMPARE(store.find(key)->effects.at(1).params.at("gainDb"), -9.0);
        QVERIFY(!store.find(key)->effects.at(1).enabled);
        devices.loadEffectPreset(0, 0, effect("gain", {{"gainDb", 1.0}})); // an EQ: not a gain preset
        QCOMPARE(store.find(key)->effects.at(0).type, std::string("eq"));
        QVERIFY(!store.find(key)->effects.at(0).params.contains("gainDb"));

        // More than a strip takes: cut to the limit.
        devices.setEffects(0, std::vector<EffectSettings>(maxEffectsPerStrip + 2, effect("gain", {{"gainDb", 0.0}})));
        QCOMPARE(store.find(key)->effects.size(), maxEffectsPerStrip);
    }
};

QTEST_GUILESS_MAIN(TestPresets)
#include "test_presets.moc"

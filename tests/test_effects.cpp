// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "effects.h"

#include <QJsonArray>
#include <QTemporaryDir>

#include <QTest>

#include <array>
#include <cmath>
#include <vector>

using namespace pp;

namespace {

constexpr double rate = 48000.0;

// Planar stereo buffers filled with a constant.
struct Buffers
{
    explicit Buffers(int frames, float value = 1.0F)
        : left(static_cast<std::size_t>(frames), value),
          right(static_cast<std::size_t>(frames), value)
    {
    }
    std::vector<float> left;
    std::vector<float> right;
    std::array<float *, 2> planes{left.data(), right.data()};
};

std::unique_ptr<EffectChain> gainChain(double db, bool enabled = true)
{
    auto gain = makeEffect("gain");
    gain->setParam("gainDb", db);
    gain->enabled = enabled;
    std::vector<std::unique_ptr<Effect>> effects;
    effects.push_back(std::move(gain));
    return std::make_unique<EffectChain>(std::move(effects), rate, 2);
}

} // namespace

class TestEffects : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void knownEffects()
    {
        QVERIFY(makeEffect("gain") != nullptr);
        QVERIFY(makeEffect("nope") == nullptr);
        const auto params = effectParams("gain");
        QCOMPARE(params.size(), 1U);
        QCOMPARE(params[0].name, "gainDb");
        QCOMPARE(params[0].initial, 0.0);
    }

    void gainSettlesSmoothly()
    {
        auto chain = gainChain(-6.0);
        // The chain starts at the set level (prepared before it runs)...
        Buffers b(4800);
        chain->process(b.planes.data(), 4800);
        QVERIFY(std::abs(b.left.back() - 0.501F) < 0.002F);
        // ...and a change glides there instead of jumping.
        chain->at(0).setParam("gainDb", 0.0);
        Buffers c(4800);
        chain->process(c.planes.data(), 4800);
        QVERIFY(c.left.front() < 0.6F);                  // not there yet at the start
        QVERIFY(std::abs(c.left.back() - 1.0F) < 0.01F); // there after 100 ms
        for (std::size_t i = 1; i < c.left.size(); ++i) {
            QVERIFY(c.left[i] - c.left[i - 1] < 0.02F); // no step
        }
    }

    void bypassIsUntouched()
    {
        auto chain = gainChain(-12.0, false);
        Buffers b(1024, 0.3F);
        chain->process(b.planes.data(), 1024);
        QVERIFY(std::ranges::all_of(b.left, [](float v) { return v == 0.3F; }));
    }

    void switchingCrossfades()
    {
        auto chain = gainChain(-20.0, false); // 0.1x when on
        chain->at(0).enabled = true;
        Buffers b(2400); // 50 ms
        chain->process(b.planes.data(), 2400);
        // Fades from dry (1.0) to wet (0.1) over ~10 ms, with no jump.
        QVERIFY(b.left.front() > 0.99F);
        QVERIFY(std::abs(b.left[600] - 0.1F) < 0.01F); // 12.5 ms in: done
        for (std::size_t i = 1; i < 600; ++i) {
            QVERIFY(std::abs(b.left[i] - b.left[i - 1]) < 0.01F);
        }
    }

    void processorPassesAndApplies()
    {
        Processor processor;
        Buffers in(256, 0.5F);
        Buffers out(256, 0.0F);
        const std::array<const float *, 2> inputs{in.left.data(), in.right.data()};
        // No chain: a copy.
        processor.process(inputs.data(), out.planes.data(), 2, 256);
        QCOMPARE(out.right[100], 0.5F);
        // A chain: applied.
        processor.setChain(gainChain(-6.0));
        processor.process(inputs.data(), out.planes.data(), 2, 256);
        QVERIFY(std::abs(out.left[255] - 0.25F) < 0.002F);
        // A chain built for another channel count: passed through, not applied.
        std::vector<std::unique_ptr<Effect>> mono;
        mono.push_back(makeEffect("gain"));
        processor.setChain(std::make_unique<EffectChain>(std::move(mono), rate, 1));
        processor.process(inputs.data(), out.planes.data(), 2, 256);
        QCOMPARE(out.left[100], 0.5F);
        // Swapping while idle frees the old chain at once; collect() is harmless.
        processor.setChain(nullptr);
        processor.collect();
        QVERIFY(processor.chain() == nullptr);
    }

    void settingsRoundTrip()
    {
        const std::vector<EffectSettings> effects{
            {.type = "gain", .enabled = false, .params = {{"gainDb", -3.0}}},
        };
        QCOMPARE(effectsFromJson(effectsToJson(effects)), effects);
        // Unknown types go; parameters are clamped, and missing ones filled in.
        QJsonArray json{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("flanger-9000")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("gain")},
                        {QStringLiteral("params"), QJsonObject{{QStringLiteral("gainDb"), 99.0}}}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("gain")}},
        };
        const auto loaded = effectsFromJson(json);
        QCOMPARE(loaded.size(), 2U);
        QCOMPARE(loaded[0].params.at("gainDb"), GainEffect::maxDb);
        QVERIFY(loaded[0].enabled);
        QCOMPARE(loaded[1].params.at("gainDb"), 0.0);
        // A scene's mix carries the chain.
        DeviceSettings s;
        s.effects = effects;
        DeviceSettings t;
        mixFromJson(t, mixToJson(s));
        QCOMPARE(t.effects, effects);
    }

    void chainUpdatesInPlace()
    {
        std::vector<EffectSettings> effects{{.type = "gain", .enabled = true, .params = {}}};
        auto chain = buildChain(effects, rate, 2);
        QCOMPARE(chain->size(), 1U);
        Effect *first = &chain->at(0);
        // Same types in the same order: updated in place (no restart, so no jump).
        effects[0].enabled = false;
        effects[0].params["gainDb"] = -6.0;
        QVERIFY(updateChain(*chain, effects));
        QCOMPARE(&chain->at(0), first);
        QVERIFY(!chain->at(0).enabled);
        // Another shape: rebuilt.
        effects.push_back({.type = "gain", .enabled = true, .params = {}});
        QVERIFY(!updateChain(*chain, effects));
        QCOMPARE(effectsNodeName("patchprime.out.maudio-1"), "patchprime.fx.out.maudio-1");
        QCOMPARE(effectsNodeName("patchprime.desktop"), "patchprime.fx.desktop");
    }

    void modelEditsChain()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel model(&store);
        model.upsertNode({.id = 10,
                          .type = "PipeWire:Interface:Node",
                          .version = 3,
                          .props = {{"media.class", "Audio/Sink"},
                                    {"node.name", "phones"},
                                    {"node.description", "Phones"},
                                    {"audio.channels", "2"}}},
                         nullptr);
        const int row = model.rowForKey("Audio/Sink|phones");
        const auto effects = [&] { return store.find("Audio/Sink|phones")->effects; };
        model.addEffect(row, QStringLiteral("gain"));
        model.addEffect(row, QStringLiteral("nope")); // unknown: ignored
        QCOMPARE(effects().size(), 1U);
        QCOMPARE(effects()[0].params.at("gainDb"), 0.0);
        model.setEffectParam(row, 0, QStringLiteral("gainDb"), -50.0); // clamped
        QCOMPARE(effects()[0].params.at("gainDb"), GainEffect::minDb);
        model.setEffectParam(row, 0, QStringLiteral("nope"), 1.0);
        QCOMPARE(effects()[0].params.size(), 1U);
        model.addEffect(row, QStringLiteral("gain"));
        model.setEffectEnabled(row, 1, false);
        model.moveEffect(row, 1, 0);
        QVERIFY(!effects()[0].enabled);
        QCOMPARE(effects()[1].params.at("gainDb"), GainEffect::minDb);
        QCOMPARE(model.data(model.index(row), DeviceModel::EffectsRole).toList().size(), 2);
        model.removeEffect(row, 5); // out of range: ignored
        model.removeEffect(row, 0);
        QCOMPARE(effects().size(), 1U);
        for (std::size_t i = 0; i < maxEffectsPerStrip + 2; ++i) {
            model.addEffect(row, QStringLiteral("gain"));
        }
        QCOMPARE(effects().size(), maxEffectsPerStrip);
    }
};

QTEST_GUILESS_MAIN(TestEffects)
#include "test_effects.moc"

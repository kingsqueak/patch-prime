// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "effects.h"
#include "dynamics.h"
#ifdef PATCHPRIME_HAVE_RNNOISE
#include "denoise.h"
#endif
#include "eq.h"
#include "multiband.h"

#include <QJsonArray>
#include <QTemporaryDir>

#include <QTest>

#include <array>
#include <cmath>
#include <random>
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

// The steady-state amplitude of a sine at freq through effect (after 0.5 s).
float sinePeak(Effect &effect, double freq, double sampleRate = rate, double amplitude = 0.25)
{
    const int frames = static_cast<int>(sampleRate); // 1 s
    std::vector<float> left(static_cast<std::size_t>(frames));
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = static_cast<float>(
            amplitude * std::sin(2.0 * M_PI * freq * static_cast<double>(i) / sampleRate));
    }
    std::vector<float> right = left;
    std::array<float *, 2> planes{left.data(), right.data()};
    for (int start = 0; start < frames; start += 1024) {
        std::array<float *, 2> block{planes[0] + start, planes[1] + start}; // NOLINT
        effect.process(block.data(), 2, std::min(1024, frames - start));
    }
    // From the RMS: at high frequencies the sample peaks miss the true peak.
    const std::size_t from = left.size() / 2;
    double sum = 0.0;
    for (std::size_t i = from; i < left.size(); ++i) {
        sum += static_cast<double>(left[i]) * left[i];
    }
    return static_cast<float>(std::sqrt(2.0 * sum / static_cast<double>(left.size() - from)));
}

double gainDb(float peak)
{
    return 20.0 * std::log10(peak / 0.25);
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
        // A block longer than one chain pass is processed all the way.
        processor.setChain(gainChain(-6.0));
        const int longFrames = (maxEffectFrames * 2) + 100;
        Buffers longIn(longFrames, 0.5F);
        Buffers longOut(longFrames, 0.0F);
        const std::array<const float *, 2> longInputs{longIn.left.data(), longIn.right.data()};
        processor.process(longInputs.data(), longOut.planes.data(), 2, longFrames);
        QVERIFY(std::abs(longOut.right.back() - 0.25F) < 0.002F);
        // Swapping while idle frees the old chain at once; collect() is harmless.
        processor.setChain(nullptr);
        processor.collect();
        QVERIFY(processor.chain() == nullptr);
    }

    void eqFlatIsTransparent()
    {
        EqEffect eq;
        eq.prepare(rate, 2);
        for (const double freq : {40.0, 1000.0, 15000.0}) {
            QVERIFY2(std::abs(gainDb(sinePeak(eq, freq))) < 0.05, qPrintable(QString::number(freq)));
        }
    }

    void eqBellAndCut()
    {
        EqEffect eq;
        eq.setParam("b3Gain", 6.0); // the 1 kHz bell
        eq.setParam("b1Type", static_cast<double>(EqEffect::BandType::LowCut));
        eq.setParam("b1Freq", 200.0);
        eq.prepare(rate, 2);
        QVERIFY(std::abs(gainDb(sinePeak(eq, 1000.0)) - 6.0) < 0.2);
        QVERIFY2(std::abs(gainDb(sinePeak(eq, 8000.0))) < 0.5, qPrintable(QString::number(gainDb(sinePeak(eq, 8000.0)))));
        QVERIFY(gainDb(sinePeak(eq, 30.0)) < -25.0); // 12 dB/octave, ~2.7 octaves down
        // The drawn curve agrees with what the filter does.
        std::vector<EqEffect::Band> bands;
        bands.reserve(EqEffect::bandCount);
        for (int b = 0; b < EqEffect::bandCount; ++b) {
            bands.push_back(EqEffect::defaultBand(b));
        }
        bands[2].gainDb = 6.0;
        bands[0].type = EqEffect::BandType::LowCut;
        bands[0].freq = 200.0;
        for (const double freq : {30.0, 200.0, 1000.0, 3000.0}) {
            EqEffect again;
            again.setParam("b3Gain", 6.0);
            again.setParam("b1Type", static_cast<double>(EqEffect::BandType::LowCut));
            again.setParam("b1Freq", 200.0);
            again.prepare(rate, 2);
            QVERIFY2(std::abs(gainDb(sinePeak(again, freq)) -
                              EqEffect::responseDb(bands, freq, rate)) < 0.3,
                     qPrintable(QString::number(freq)));
        }
    }

    void eqGlidesAndSwitchesQuietly()
    {
        EqEffect eq;
        eq.prepare(rate, 2);
        // A steady sine; change gain, type and on/off mid-stream: the output
        // never jumps far beyond the boosted level, and stays finite.
        constexpr int frames = 512 * 96; // about 1 s, in whole blocks
        std::vector<float> left(frames);
        for (std::size_t i = 0; i < left.size(); ++i) {
            left[i] = static_cast<float>(0.25 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / rate));
        }
        std::vector<float> right = left;
        float previous = 0.0F;
        float worstStep = 0.0F;
        for (int start = 0; start < frames; start += 512) {
            if (start == 4096) {
                eq.setParam("b3Gain", 12.0);
            }
            if (start == 16384) {
                eq.setParam("b3Type", static_cast<double>(EqEffect::BandType::HighShelf));
            }
            if (start == 32768) {
                eq.setParam("b3On", 0.0);
            }
            std::array<float *, 2> block{left.data() + start, right.data() + start}; // NOLINT
            eq.process(block.data(), 2, 512);
            for (int i = start; i < start + 512; ++i) {
                const float v = left[static_cast<std::size_t>(i)];
                QVERIFY(std::isfinite(v));
                worstStep = std::max(worstStep, std::abs(v - previous));
                previous = v;
            }
        }
        // A 1 kHz sine at 4x (+12 dB) moves at most ~0.13 per sample at 48 kHz.
        QVERIFY2(worstStep < 0.2F, qPrintable(QString::number(worstStep)));
    }

    void eqExtremesStayStable()
    {
        EqEffect eq;
        for (int b = 1; b <= EqEffect::bandCount; ++b) {
            const auto p = "b" + std::to_string(b);
            eq.setParam(p + "Freq", 20000.0);
            eq.setParam(p + "Gain", 24.0);
            eq.setParam(p + "Q", 10.0);
        }
        eq.prepare(44100.0, 2); // 20 kHz is above 0.45 x 44.1 kHz: clamped
        const float peak = sinePeak(eq, 15000.0, 44100.0);
        QVERIFY(std::isfinite(peak));
        QVERIFY(EqEffect::coefficients({.on = true,
                                        .type = EqEffect::BandType::HighCut,
                                        .freq = 5.0, // below range: clamped
                                        .gainDb = 0.0,
                                        .q = 0.0},
                                       rate)
                    .b0 > 0.0);
        // Parameters by kind.
        const auto params = effectParams("eq");
        QCOMPARE(params.size(), 30U);
        QCOMPARE(params[1].kind, ParamKind::Choice);
        QCOMPARE(params[1].options.size(), 5U);
        QCOMPARE(validParam(params[1], 2.6), 3.0);
        QCOMPARE(validParam(params[2], 1e9), EqEffect::maxFreq);
    }

    void multibandSumsFlat()
    {
        // Ratio 1:1 everywhere: nothing compresses, and the four bands add
        // back up to the input at every frequency, crossovers included.
        MultibandEffect mb;
        for (int b = 1; b <= MultibandEffect::bandCount; ++b) {
            mb.setParam("b" + std::to_string(b) + "Ratio", 1.0);
        }
        mb.prepare(rate, 2);
        for (const double freq : {40.0, 120.0, 400.0, 1000.0, 3000.0, 6000.0, 12000.0}) {
            QVERIFY2(std::abs(gainDb(sinePeak(mb, freq))) < 0.1, qPrintable(QString::number(freq)));
        }
        // Moved crossovers (out of order, even): still flat.
        mb.setParam("x1", 500.0);
        mb.setParam("x2", 400.0);
        mb.setParam("x3", 2000.0);
        for (const double freq : {400.0, 500.0, 2000.0}) {
            QVERIFY2(std::abs(gainDb(sinePeak(mb, freq))) < 0.1, qPrintable(QString::number(freq)));
        }
    }

    void multibandCompressesOneBand()
    {
        MultibandEffect mb;
        for (int b = 1; b <= MultibandEffect::bandCount; ++b) {
            mb.setParam("b" + std::to_string(b) + "Ratio", 1.0);
        }
        // Band 3: -20 dB threshold, 4:1. A 2 kHz tone peaking at
        // -6 dBFS is -9 dB RMS, 11 dB over: it comes out 11 x 3/4 = 8.3 dB down.
        // Band 3 made wide (200 Hz - 16 kHz) so the whole tone is in it.
        mb.setParam("x2", 200.0);
        mb.setParam("x3", 16000.0);
        mb.setParam("b3Threshold", -20.0);
        mb.setParam("b3Ratio", 4.0);
        mb.prepare(rate, 2);
        const double loud = 20.0 * std::log10(sinePeak(mb, 2000.0, rate, 0.5) / 0.5);
        QVERIFY2(std::abs(loud + 8.27) < 0.5, qPrintable(QString::number(loud)));
        std::array<float, 4> meters{};
        QCOMPARE(mb.takeMeters(meters), 4U);
        QVERIFY2(std::abs(meters[2] - 8.27F) < 0.7F, qPrintable(QString::number(meters[2])));
        QCOMPARE(meters[0], 0.0F);
        // Another band's tone is left alone.
        QVERIFY(std::abs(20.0 * std::log10(sinePeak(mb, 60.0, rate, 0.5) / 0.5)) < 0.3);
        // Makeup on band 3 brings it back up.
        mb.setParam("b3Makeup", 8.27);
        const double made = 20.0 * std::log10(sinePeak(mb, 2000.0, rate, 0.5) / 0.5);
        QVERIFY2(std::abs(made) < 0.5, qPrintable(QString::number(made)));
        // Solo band 1: the 2 kHz tone is gone.
        mb.setParam("b1Solo", 1.0);
        QVERIFY(gainDb(sinePeak(mb, 2000.0)) < -40.0);
        QVERIFY(std::abs(gainDb(sinePeak(mb, 30.0))) < 0.3); // two octaves under x1: all band 1
    }

    void compressorCurve()
    {
        // Below the knee: unchanged; above: the ratio; the knee joins them.
        QCOMPARE(MultibandEffect::staticCurve(-40.0, -20.0, 4.0), -40.0);
        QCOMPARE(MultibandEffect::staticCurve(-4.0, -20.0, 4.0), -16.0);
        const double edge = MultibandEffect::kneeDb / 2.0;
        QVERIFY(std::abs(MultibandEffect::staticCurve(-20.0 - edge, -20.0, 4.0) - (-20.0 - edge)) <
                1e-9);
        QVERIFY(std::abs(MultibandEffect::staticCurve(-20.0 + edge, -20.0, 4.0) -
                         (-20.0 + (edge / 4.0))) < 1e-9);
        QCOMPARE(effectParams("multiband").size(), 31U);
    }

    void gateClosesOnQuiet()
    {
        GateEffect gate; // threshold -50 dB, range 40 dB
        gate.prepare(rate, 2);
        // Quiet noise floor (about -66 dBFS): turned down by the range.
        const double quiet = 20.0 * std::log10(sinePeak(gate, 1000.0, rate, 0.0005) / 0.0005);
        QVERIFY2(quiet < -35.0, qPrintable(QString::number(quiet)));
        std::array<float, 1> meter{};
        QCOMPARE(gate.takeMeters(meter), 1U);
        QVERIFY(meter[0] > 35.0F);
        // Speech level: passes untouched.
        QVERIFY(std::abs(gainDb(sinePeak(gate, 1000.0))) < 0.1);
    }

    void duckerFollowsKey()
    {
        DuckerEffect ducker; // threshold -35 dB, depth 12 dB
        ducker.prepare(rate, 2);
        // No key: nothing happens.
        QVERIFY(std::abs(gainDb(sinePeak(ducker, 1000.0))) < 0.01);
        // A loud key (a voice at about -10 dBFS): down by the depth.
        std::vector<float> key(48000);
        for (std::size_t i = 0; i < key.size(); ++i) {
            key[i] = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 200.0 * static_cast<double>(i) / rate));
        }
        Buffers music(48000, 0.5F);
        for (int start = 0; start < 48000; start += 480) {
            std::array<float *, 2> block{music.left.data() + start, music.right.data() + start}; // NOLINT
            ducker.setKey(key.data() + start); // NOLINT
            ducker.process(block.data(), 2, 480);
        }
        const double dipped = 20.0 * std::log10(music.left.back() / 0.5);
        QVERIFY2(std::abs(dipped + 12.0) < 0.3, qPrintable(QString::number(dipped)));
        std::array<float, 1> meter{};
        ducker.takeMeters(meter);
        QVERIFY(std::abs(meter[0] - 12.0F) < 0.3F);
        // The key stops: back up after hold and release (well within 4 s).
        ducker.setKey(nullptr);
        Buffers after(48000 * 4, 0.5F);
        ducker.process(after.planes.data(), 2, 48000 * 4);
        QVERIFY(std::abs(20.0 * std::log10(after.left.back() / 0.5)) < 0.1);
        QVERIFY(after.left[2400] < 0.2F); // still down 50 ms in (held)
    }

#ifdef PATCHPRIME_HAVE_RNNOISE
    void denoiseRemovesNoise()
    {
        // Steady white noise at about -30 dBFS is not a voice: mostly removed.
        DenoiseEffect denoise;
        denoise.prepare(rate, 1);
        std::mt19937 random(1);
        std::uniform_real_distribution<float> noise(-0.05F, 0.05F);
        std::vector<float> samples(static_cast<std::size_t>(48000) * 2);
        for (float &s : samples) {
            s = noise(random);
        }
        const auto rms = [](std::span<const float> part) {
            double sum = 0.0;
            for (const float s : part) {
                sum += static_cast<double>(s) * s;
            }
            return std::sqrt(sum / static_cast<double>(part.size()));
        };
        const double before = rms(std::span(samples).subspan(48000));
        for (std::size_t start = 0; start < samples.size(); start += 256) {
            float *plane = samples.data() + start; // NOLINT
            denoise.process(&plane, 1, 256);
        }
        const double after = rms(std::span(samples).subspan(48000));
        QVERIFY2(20.0 * std::log10(after / before) < -10.0,
                 qPrintable(QString::number(20.0 * std::log10(after / before))));

        // Amount 0: the original, 10 ms (480 samples) late.
        DenoiseEffect dry;
        dry.setParam("amount", 0.0);
        dry.prepare(rate, 1);
        std::vector<float> ramp(2000);
        for (std::size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = static_cast<float>(i) / 4000.0F;
        }
        std::vector<float> out = ramp;
        float *plane = out.data();
        dry.process(&plane, 1, static_cast<int>(out.size()));
        QCOMPARE(out[100], 0.0F);
        QVERIFY(std::abs(out[1500] - ramp[1500 - DenoiseEffect::frameSize]) < 1e-6F);

        // Not at 48 kHz: passed through.
        DenoiseEffect other;
        other.prepare(44100.0, 1);
        std::vector<float> same = ramp;
        plane = same.data();
        other.process(&plane, 1, static_cast<int>(same.size()));
        QCOMPARE(same, ramp);
    }
#endif

    void keyLoopsAreFound()
    {
        const std::map<std::string, std::vector<std::string>> sends{
            {"mic", {"mix"}}, {"mix", {"out"}}, {"music", {"mix"}}};
        QVERIFY(reaches(sends, "mic", "out"));   // via the mix
        QVERIFY(!reaches(sends, "music", "mic"));
        QVERIFY(!reaches(sends, "out", "mic"));
        // Music ducked by the mic: fine. The mix ducked by the mic: fine
        // (the key reads the mic). The mic ducked by the mix: a loop.
        QVERIFY(reaches(sends, "mic", "mix"));
        QVERIFY(effectTakesKey("ducker") && !effectTakesKey("gate"));
    }

    void settingsRoundTrip()
    {
        const std::vector<EffectSettings> effects{
            {.type = "gain", .enabled = false, .params = {{"gainDb", -3.0}}, .key = {}},
            {.type = "ducker",
             .enabled = true,
             .params = {{"threshold", -30.0}, {"depth", 10.0}, {"attack", 20.0}, {"hold", 250.0},
                        {"release", 600.0}},
             .key = "Audio/Source|mic"},
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
        std::vector<EffectSettings> effects{{.type = "gain", .enabled = true, .params = {}, .key = {}}};
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
        effects.push_back({.type = "gain", .enabled = true, .params = {}, .key = {}});
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

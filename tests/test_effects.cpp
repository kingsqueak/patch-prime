// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "effects.h"
#include "delay.h"
#include "dynamics.h"
#ifdef PATCHPRIME_HAVE_RNNOISE
#include "denoise.h"
#endif
#include "eq.h"
#include "loudness.h"
#include "multiband.h"
#include "reverb.h"

#include <QJsonArray>
#include <QTemporaryDir>

#include <QTest>

#include <array>
#include <cmath>
#include <limits>
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

    void reviewFixes()
    {
        // An EQ band switched off and on again doesn't bring back old sound.
        EqEffect eq;
        eq.setParam("b3Gain", 24.0);
        eq.setParam("b3Q", 10.0);
        eq.prepare(rate, 2);
        sinePeak(eq, 1000.0, rate, 1.0);
        eq.setParam("b3On", 0.0);
        Buffers quiet(4800, 0.0F);
        eq.process(quiet.planes.data(), 2, 4800); // fades out
        eq.setParam("b3On", 1.0);
        Buffers after(4800, 0.0F);
        eq.process(after.planes.data(), 2, 4800);
        QVERIFY(std::ranges::all_of(after.left, [](float v) { return v == 0.0F; }));

        // A multiband band turned off is uncompressed within milliseconds,
        // whatever its release time.
        MultibandEffect mb;
        for (int b = 1; b <= 4; ++b) {
            mb.setParam("b" + std::to_string(b) + "Threshold", -40.0);
            mb.setParam("b" + std::to_string(b) + "Ratio", 20.0);
            mb.setParam("b" + std::to_string(b) + "Release", 3000.0);
        }
        mb.prepare(rate, 2);
        QVERIFY(gainDb(sinePeak(mb, 30.0, rate, 0.5)) < -20.0);
        mb.setParam("b1On", 0.0);
        Buffers settle(2400, 0.0F); // 50 ms
        mb.process(settle.planes.data(), 2, 2400);
        std::array<float, 4> meters{};
        mb.takeMeters(meters);
        mb.takeMeters(meters); // only what's new
        Buffers probe(480, 0.0F);
        mb.process(probe.planes.data(), 2, 480);
        mb.takeMeters(meters);
        QVERIFY2(meters[0] < 0.1F, qPrintable(QString::number(meters[0])));

        // A NaN from an app doesn't stick in a chain.
        Processor processor;
        std::vector<std::unique_ptr<Effect>> effects;
        effects.push_back(makeEffect("eq"));
        effects.push_back(makeEffect("reverb"));
        processor.setChain(std::make_unique<EffectChain>(std::move(effects), rate, 2));
        Buffers bad(256, 0.1F);
        bad.left[10] = std::numeric_limits<float>::quiet_NaN();
        bad.right[20] = std::numeric_limits<float>::infinity();
        const std::array<const float *, 2> in{bad.left.data(), bad.right.data()};
        Buffers out(256, 0.0F);
        processor.process(in.data(), out.planes.data(), 2, 256);
        Buffers fine(4800, 0.1F);
        const std::array<const float *, 2> in2{fine.left.data(), fine.right.data()};
        Buffers out2(4800, 0.0F);
        processor.process(in2.data(), out2.planes.data(), 2, 4800);
        QVERIFY(std::ranges::all_of(out.left, [](float v) { return std::isfinite(v); }));
        QVERIFY(std::ranges::all_of(out2.right, [](float v) { return std::isfinite(v); }));
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

    void loudnessReference()
    {
        // BS.1770: a 0 dBFS 997 Hz sine in one channel reads -3.01 LUFS.
        LevelerEffect meter;
        meter.setParam("maxBoost", 0.0);
        meter.setParam("maxCut", 0.0);
        meter.prepare(rate, 1);
        std::vector<float> tone(static_cast<std::size_t>(48000) * 5);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            tone[i] = static_cast<float>(std::sin(2.0 * M_PI * 997.0 * static_cast<double>(i) / rate));
        }
        float *plane = tone.data();
        meter.process(&plane, 1, static_cast<int>(tone.size()));
        std::array<float, 2> values{};
        QCOMPARE(meter.takeMeters(values), 2U);
        QVERIFY2(std::abs(values[1] + 3.01F) < 0.1F, qPrintable(QString::number(values[1])));
        QCOMPARE(values[0], 0.0F);
    }

    void levelerRidesToTarget()
    {
        // A quiet stereo tone at about -30 LUFS, target -18: boosted 12 dB.
        const auto run = [](LevelerEffect &leveler, double amplitude, int seconds) {
            std::vector<float> left(static_cast<std::size_t>(48000 * seconds));
            for (std::size_t i = 0; i < left.size(); ++i) {
                left[i] = static_cast<float>(amplitude * std::sin(2.0 * M_PI * 997.0 * static_cast<double>(i) / rate));
            }
            std::vector<float> right = left;
            for (std::size_t start = 0; start < left.size(); start += 512) {
                std::array<float *, 2> block{left.data() + start, right.data() + start}; // NOLINT
                leveler.process(block.data(), 2, static_cast<int>(std::min<std::size_t>(512, left.size() - start)));
            }
            std::array<float, 2> values{};
            leveler.takeMeters(values);
            return values;
        };
        // Stereo 997 Hz at amplitude a: 20 log10(a) + 3.01 - 3.01 = 20 log10(a) LUFS.
        const double amplitude = std::pow(10.0, -30.0 / 20.0);
        LevelerEffect leveler;
        leveler.setParam("speed", 1.0);
        leveler.setParam("maxBoost", 24.0);
        leveler.prepare(rate, 2);
        auto values = run(leveler, amplitude, 12);
        QVERIFY2(std::abs(values[1] + 30.0F) < 0.3F, qPrintable(QString::number(values[1])));
        QVERIFY2(std::abs(values[0] - 12.0F) < 0.5F, qPrintable(QString::number(values[0])));
        // Silence: the gain holds where it was (no pumping up the noise).
        values = run(leveler, 0.0, 6);
        QVERIFY2(std::abs(values[0] - 12.0F) < 1.0F, qPrintable(QString::number(values[0])));
        // Max boost limits it.
        LevelerEffect capped;
        capped.setParam("speed", 1.0);
        capped.setParam("maxBoost", 6.0);
        capped.prepare(rate, 2);
        values = run(capped, amplitude, 12);
        QVERIFY2(std::abs(values[0] - 6.0F) < 0.3F, qPrintable(QString::number(values[0])));
    }

    void limiterHoldsTheCeiling()
    {
        const float ceiling = std::pow(10.0F, -1.0F / 20.0F);
        // A full-scale tone: nothing over the ceiling, and close to it.
        LimiterEffect limiter;
        limiter.prepare(rate, 2);
        Buffers tone(48000, 0.0F);
        for (std::size_t i = 0; i < tone.left.size(); ++i) {
            tone.left[i] = tone.right[i] =
                static_cast<float>(std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / rate));
        }
        limiter.process(tone.planes.data(), 2, 48000);
        float peak = 0.0F;
        for (const float v : tone.left) {
            peak = std::max(peak, std::abs(v));
        }
        QVERIFY(peak <= ceiling);
        QVERIFY(peak > ceiling * 0.95F);
        std::array<float, 1> meter{};
        limiter.takeMeters(meter);
        QVERIFY(std::abs(meter[0] - 1.0F) < 0.2F);

        // A lone spike in quiet: caught before it arrives, not clipped flat.
        LimiterEffect spiky;
        spiky.prepare(rate, 2);
        Buffers spike(4800, 0.1F);
        spike.left[2000] = spike.right[2000] = 1.0F;
        spiky.process(spike.planes.data(), 2, 4800);
        const int delay = static_cast<int>(std::lround(LimiterEffect::lookaheadSeconds * rate));
        QVERIFY(spike.left[static_cast<std::size_t>(2000 + delay)] <= ceiling);
        QVERIFY(spike.left[static_cast<std::size_t>(2000 + delay)] > ceiling * 0.99F);
        // Quieter audio passes unchanged, lookahead later.
        QCOMPARE(spike.left[1000], 0.1F);
        QCOMPARE(spike.left[static_cast<std::size_t>(delay - 1)], 0.0F);
        QCOMPARE(spike.left[static_cast<std::size_t>(delay)], 0.1F);
    }

    void reverbDecaysAtItsTime()
    {
        // Mix 0: untouched.
        ReverbEffect dry;
        dry.setParam("mix", 0.0);
        dry.prepare(rate, 2);
        Buffers same(4800, 0.3F);
        dry.process(same.planes.data(), 2, 4800);
        QVERIFY(std::ranges::all_of(same.left, [](float v) { return v == 0.3F; }));

        // An impulse into a 2 s reverb, fully wet: the tail falls about
        // 30 dB in a second, and is gone after several.
        ReverbEffect reverb;
        reverb.setParam("mix", 100.0);
        reverb.setParam("decay", 2.0);
        reverb.setParam("predelay", 0.0);
        reverb.prepare(rate, 2);
        constexpr std::size_t seconds = 8;
        Buffers tail(static_cast<int>(48000 * seconds), 0.0F);
        tail.left[0] = tail.right[0] = 1.0F;
        for (std::size_t start = 0; start < tail.left.size(); start += 480) {
            std::array<float *, 2> block{tail.left.data() + start, tail.right.data() + start}; // NOLINT
            reverb.process(block.data(), 2, 480);
        }
        const auto rmsDb = [&](double from, double to) {
            double sum = 0.0;
            const auto a = static_cast<std::size_t>(from * rate);
            const auto b = static_cast<std::size_t>(to * rate);
            for (std::size_t i = a; i < b; ++i) {
                sum += static_cast<double>(tail.left[i]) * tail.left[i];
            }
            return 10.0 * std::log10((sum / static_cast<double>(b - a)) + 1e-30);
        };
        const double drop = rmsDb(0.3, 0.4) - rmsDb(1.3, 1.4);
        QVERIFY2(std::abs(drop - 30.0) < 6.0, qPrintable(QString::number(drop)));
        QVERIFY(rmsDb(7.0, 8.0) < rmsDb(0.3, 0.4) - 90.0);
        QVERIFY(std::ranges::all_of(tail.left, [](float v) { return std::isfinite(v) && std::abs(v) < 2.0F; }));
        // Left and right differ (a stereo tail)...
        double difference = 0.0;
        for (std::size_t i = 4800; i < 48000; ++i) {
            difference += std::abs(tail.left[i] - tail.right[i]);
        }
        QVERIFY(difference > 1.0);
        // ...and width 0 makes it mono.
        ReverbEffect narrow;
        narrow.setParam("mix", 100.0);
        narrow.setParam("width", 0.0);
        narrow.prepare(rate, 2);
        Buffers mono(9600, 0.0F);
        mono.left[0] = mono.right[0] = 1.0F;
        narrow.process(mono.planes.data(), 2, 9600);
        QCOMPARE(mono.left, mono.right);
    }

    void reverbStaysBounded()
    {
        // The longest decay, the biggest room, loud noise for 10 s.
        ReverbEffect reverb;
        reverb.setParam("mix", 100.0);
        reverb.setParam("decay", 10.0);
        reverb.setParam("size", 100.0);
        reverb.setParam("damping", 0.0);
        reverb.prepare(rate, 2);
        std::mt19937 random(2);
        std::uniform_real_distribution<float> noise(-0.5F, 0.5F);
        float peak = 0.0F;
        for (int block = 0; block < 1000; ++block) {
            Buffers b(480, 0.0F);
            for (std::size_t i = 0; i < b.left.size(); ++i) {
                b.left[i] = noise(random);
                b.right[i] = noise(random);
            }
            if (block == 500) {
                reverb.setParam("size", 0.0); // a size change mid-stream
            }
            reverb.process(b.planes.data(), 2, 480);
            for (const float v : b.left) {
                QVERIFY(std::isfinite(v));
                peak = std::max(peak, std::abs(v));
            }
        }
        QVERIFY2(peak < 8.0F, qPrintable(QString::number(peak)));
    }

    void delayLinesUp()
    {
        // 0 ms: untouched.
        DelayEffect none;
        none.prepare(rate, 2);
        Buffers same(1000, 0.3F);
        none.process(same.planes.data(), 2, 1000);
        QVERIFY(std::ranges::all_of(same.left, [](float v) { return v == 0.3F; }));
        // 10 ms: an impulse comes out 480 samples later.
        DelayEffect late;
        late.setParam("time", 10.0);
        late.prepare(rate, 2);
        Buffers impulse(2000, 0.0F);
        impulse.left[5] = 1.0F;
        late.process(impulse.planes.data(), 2, 2000);
        QCOMPARE(impulse.left[485], 1.0F);
        QCOMPARE(std::ranges::count_if(impulse.left, [](float v) { return v != 0.0F; }), 1);
        // Changing the time on a running tone: a crossfade, not a jump.
        DelayEffect moving;
        moving.prepare(rate, 1);
        std::vector<float> tone(48000);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            tone[i] = static_cast<float>(0.5 * std::sin(2.0 * M_PI * 100.0 * static_cast<double>(i) / rate));
        }
        float previous = 0.0F;
        float worst = 0.0F;
        for (std::size_t start = 0; start < tone.size(); start += 480) {
            if (start == 9600) {
                moving.setParam("time", 3.3);
            }
            float *plane = tone.data() + start; // NOLINT
            moving.process(&plane, 1, 480);
            for (std::size_t i = start; i < start + 480; ++i) {
                worst = std::max(worst, std::abs(tone[i] - previous));
                previous = tone[i];
            }
        }
        // A 100 Hz tone at 0.5 moves at most ~0.0066 per sample.
        QVERIFY2(worst < 0.02F, qPrintable(QString::number(worst)));
    }

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

    void cueChoices()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel model(&store);
        const auto node = [](std::uint32_t id, const std::string &cls, const std::string &name) {
            return GraphObject{.id = id,
                               .type = "PipeWire:Interface:Node",
                               .version = 3,
                               .props = {{"media.class", cls},
                                         {"node.name", name},
                                         {"node.description", name},
                                         {"audio.channels", "2"}}};
        };
        model.upsertNode(node(10, "Audio/Sink", "phones"), nullptr);
        model.upsertNode(node(11, "Audio/Source", "mic"), nullptr);
        const auto mic = [&] { return model.rowForKey("Audio/Source|mic"); };
        model.setEnabled(mic(), true);
        model.setEnabled(model.rowForKey("Audio/Sink|phones"), true);
        // No cue output yet: nothing can be cued.
        model.setCue(mic(), true);
        QVERIFY(model.cued().empty());
        // Only a hardware output can be the cue output.
        model.setCueOutput("Audio/Source|mic");
        QVERIFY(store.cueOutput().empty());
        model.setCueOutput("Audio/Sink|phones");
        QCOMPARE(store.cueOutput(), "Audio/Sink|phones");
        model.setCue(mic(), true);
        model.setCue(model.rowForKey("Audio/Sink|phones"), true); // not itself
        QCOMPARE(model.cued(), std::set<std::string>{"Audio/Source|mic"});
        QVERIFY(model.data(model.index(mic()), DeviceModel::CuedRole).toBool());
        // Saved: the output, not what's cued.
        QVERIFY(store.save());
        DeviceStore again(store.path());
        QVERIFY(again.load());
        QCOMPARE(again.cueOutput(), "Audio/Sink|phones");
        // Cue off: every cue cleared.
        model.setCueOutput({});
        QVERIFY(model.cued().empty());
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

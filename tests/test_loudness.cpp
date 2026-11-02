// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "loudness.h"
#include "loudnessmeter.h"

#include <QTest>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

// Feeds a stereo sine (the same in both channels) of `seconds` at `dbfs`
// (peak) through a meter into an analysis.
void play(pp::LoudnessMeter &meter, pp::LoudnessAnalysis &analysis, double seconds, double dbfs,
          double hz = 997.0, int rate = 48000)
{
    static double phase = 0.0;
    const double amplitude = std::pow(10.0, dbfs / 20.0);
    const int frames = static_cast<int>(seconds * rate);
    std::vector<float> buffer(std::size_t{480} * 2);
    for (int done = 0; done < frames; done += 480) {
        for (std::size_t i = 0; i < 480; ++i) {
            const auto x = static_cast<float>(amplitude * std::sin(phase));
            phase += 2.0 * std::numbers::pi * hz / rate;
            buffer[i * 2] = x;
            buffer[(i * 2) + 1] = x;
        }
        meter.process(buffer.data(), 480, 2);
        for (const auto &b : meter.take()) {
            analysis.add(b);
        }
    }
}

} // namespace

class TestLoudness : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A sine at a quarter of the rate, 45 degrees off: every sample is at
    // 0.707, but the wave peaks at 1 between them.
    void truePeakFindsPeaksBetweenSamples()
    {
        pp::TruePeakDetector detector;
        float samplePeak = 0.0F;
        float truePeak = 0.0F;
        for (int i = 0; i < 400; ++i) {
            const auto x = static_cast<float>(std::sin((std::numbers::pi / 2.0 * i) + (std::numbers::pi / 4.0)));
            samplePeak = std::max(samplePeak, std::abs(x));
            if (i > 20) {
                truePeak = std::max(truePeak, detector.process(x));
            } else {
                detector.process(x);
            }
        }
        QVERIFY2(samplePeak < 0.71F, qPrintable(QString::number(samplePeak)));
        QVERIFY2(truePeak > 0.95F && truePeak < 1.05F, qPrintable(QString::number(truePeak)));
    }

    // EBU Tech 3341 case 1: stereo 1 kHz at -23 dBFS reads -23 LUFS.
    void readsTheReferenceTone()
    {
        pp::LoudnessMeter meter;
        meter.setRate(48000);
        pp::LoudnessAnalysis analysis;
        QVERIFY(std::isinf(analysis.integrated()));
        play(meter, analysis, 20.0, -23.0);
        QVERIFY2(std::abs(analysis.integrated() + 23.0) < 0.1, qPrintable(QString::number(analysis.integrated())));
        QVERIFY(std::abs(analysis.momentary() + 23.0) < 0.1);
        QVERIFY(std::abs(analysis.shortTerm() + 23.0) < 0.1);
        QVERIFY(std::abs(analysis.truePeakDb() + 23.0) < 0.2);
        QVERIFY(std::abs(analysis.seconds() - 20.0) < 0.15);
        analysis.reset();
        QVERIFY(std::isinf(analysis.integrated()));
        QCOMPARE(analysis.seconds(), 0.0);
    }

    // Tech 3341 case 4: quiet parts 26 dB under are gated out: -23 LUFS.
    void gatesQuietParts()
    {
        pp::LoudnessMeter meter;
        meter.setRate(48000);
        pp::LoudnessAnalysis analysis;
        play(meter, analysis, 10.0, -72.0); // under the absolute gate
        play(meter, analysis, 10.0, -36.0);
        play(meter, analysis, 60.0, -23.0);
        play(meter, analysis, 10.0, -36.0);
        play(meter, analysis, 10.0, -72.0);
        QVERIFY2(std::abs(analysis.integrated() + 23.0) < 0.1, qPrintable(QString::number(analysis.integrated())));
    }

    // Tech 3342 case 1: 20 s at -20 then 20 s at -30 dBFS: a range of 10 LU.
    void measuresTheRange()
    {
        pp::LoudnessMeter meter;
        meter.setRate(48000);
        pp::LoudnessAnalysis analysis;
        play(meter, analysis, 20.0, -20.0);
        play(meter, analysis, 20.0, -30.0);
        QVERIFY2(std::abs(analysis.range() - 10.0) < 1.0, qPrintable(QString::number(analysis.range())));
    }

    // The limiter in true-peak mode holds the same quarter-rate sine (peaks
    // between samples) under its ceiling; in sample-peak mode it doesn't.
    void limiterHoldsTruePeaks()
    {
        for (const bool truePeak : {false, true}) {
            pp::LimiterEffect limiter;
            limiter.prepare(48000.0, 1);
            limiter.setParam("ceiling", -6.0);
            limiter.setParam("truePeak", truePeak ? 1.0 : 0.0);
            std::vector<float> wave(48000);
            for (std::size_t i = 0; i < wave.size(); ++i) {
                wave[i] = static_cast<float>(0.8 * std::sin((std::numbers::pi / 2.0 * static_cast<double>(i)) + (std::numbers::pi / 4.0)));
            }
            std::array<float *, 1> planes{wave.data()};
            limiter.process(planes.data(), 1, static_cast<int>(wave.size()));
            pp::TruePeakDetector detector;
            float peak = 0.0F;
            for (std::size_t i = 0; i < wave.size(); ++i) {
                const float p = detector.process(wave[i]);
                if (i > 24000) {
                    peak = std::max(peak, p);
                }
            }
            const double db = 20.0 * std::log10(peak);
            if (truePeak) {
                QVERIFY2(db < -5.9, qPrintable(QString::number(db)));
            } else {
                QVERIFY2(db > -3.5, qPrintable(QString::number(db))); // samples at -6, the wave 3 dB over
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestLoudness)
#include "test_loudness.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "loudness.h"
#include "pipewiregraph.h"
#include "recording.h"

#include <cstddef>
#include <deque>
#include <span>
#include <vector>

namespace pp {

// One 100 ms block of a loudness measurement: the K-weighted mean square,
// summed over the channels, and the block's true peak (linear).
struct LoudnessBlock
{
    double meanSquare = 0.0;
    double truePeak = 0.0;
};

// Loudness for publishing (ITU-R BS.1770-4, EBU R128 / Tech 3341 and 3342)
// from 100 ms blocks: momentary (400 ms), short-term (3 s), integrated
// (gated: absolute at -70 LUFS, relative 10 LU under), loudness range (LRA:
// short-term values gated at -70 and 20 LU under, 10th to 95th percentile)
// and the true peak, since the last reset. Readings that don't exist yet
// (too little measured, or only silence) are -infinity.
class LoudnessAnalysis
{
public:
    static constexpr double blockSeconds = 0.1;

    void add(const LoudnessBlock &block);
    void reset();

    [[nodiscard]] double momentary() const;
    [[nodiscard]] double shortTerm() const;
    [[nodiscard]] double integrated() const;
    [[nodiscard]] double range() const; // LU
    [[nodiscard]] double truePeakDb() const;
    [[nodiscard]] double seconds() const { return static_cast<double>(m_blocks) * blockSeconds; }

private:
    std::deque<double> m_recent;        // the last 30 blocks' mean squares
    std::vector<double> m_gatingBlocks; // 400 ms mean squares, every 100 ms
    std::vector<double> m_shortTerms;   // 3 s mean squares, every 100 ms
    double m_truePeak = 0.0;
    std::size_t m_blocks = 0;
};

// The realtime side: a capture stream's io that K-weights its channels and
// hands over a LoudnessBlock every 100 ms (lock-free), with the true peak.
class LoudnessMeter : public StreamIo
{
public:
    LoudnessMeter();

    void setRate(int rate) noexcept override;
    void process(float *samples, std::uint32_t frames, std::uint32_t channelCount) noexcept override;

    // Main thread: the blocks finished since the last call.
    std::vector<LoudnessBlock> take();

private:
    SampleRing m_ring; // two values per block
    // Realtime only.
    int m_rate = 0;
    KWeighting m_weighting;
    std::array<std::array<double, 2>, maxEffectChannels> m_shelfState{};
    std::array<std::array<double, 2>, maxEffectChannels> m_passState{};
    std::array<TruePeakDetector, maxEffectChannels> m_detectors{};
    double m_sum = 0.0;
    double m_peak = 0.0;
    int m_count = 0;
    int m_blockLength = 4800;
};

} // namespace pp

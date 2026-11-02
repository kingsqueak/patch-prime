// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "loudnessmeter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace pp {

namespace {

constexpr double absoluteGate = -70.0;   // LUFS
constexpr double relativeGate = -10.0;   // LU, integrated
constexpr double rangeGate = -20.0;      // LU, loudness range
constexpr std::size_t momentaryBlocks = 4;
constexpr std::size_t shortTermBlocks = 30;
constexpr double none = -std::numeric_limits<double>::infinity();

inline double step(const EqEffect::Coefficients &c, std::array<double, 2> &z, double x) noexcept
{
    const double y = (c.b0 * x) + z[0];
    z[0] = (c.b1 * x) - (c.a1 * y) + z[1];
    z[1] = (c.b2 * x) - (c.a2 * y);
    return y;
}

double meanOfLast(const std::deque<double> &values, std::size_t count)
{
    if (values.size() < count) {
        return -1.0;
    }
    return std::accumulate(values.end() - static_cast<std::ptrdiff_t>(count), values.end(), 0.0) /
           static_cast<double>(count);
}

double lufsOrNone(double meanSquare)
{
    return meanSquare < 0.0 ? none : lufsFromMeanSquare(meanSquare);
}

// The mean square of the blocks louder than gate (LUFS); -1 when none are.
double meanAbove(const std::vector<double> &blocks, double gate)
{
    double sum = 0.0;
    std::size_t count = 0;
    for (const double b : blocks) {
        if (lufsFromMeanSquare(b) > gate) {
            sum += b;
            ++count;
        }
    }
    return count == 0 ? -1.0 : sum / static_cast<double>(count);
}

// The mean square of the blocks above the absolute gate and `relative` LU
// under their own loudness; -1 when none are.
double gatedMean(const std::vector<double> &blocks, double relative)
{
    const double first = meanAbove(blocks, absoluteGate);
    if (first < 0.0) {
        return -1.0;
    }
    return meanAbove(blocks, std::max(absoluteGate, lufsFromMeanSquare(first) + relative));
}

} // namespace

// --- LoudnessAnalysis ----------------------------------------------------------

void LoudnessAnalysis::add(const LoudnessBlock &block)
{
    ++m_blocks;
    m_truePeak = std::max(m_truePeak, block.truePeak);
    m_recent.push_back(block.meanSquare);
    if (m_recent.size() > shortTermBlocks) {
        m_recent.pop_front();
    }
    // Gating blocks overlap by 75%: one every 100 ms once there are four.
    if (const double m = meanOfLast(m_recent, momentaryBlocks); m >= 0.0) {
        m_gatingBlocks.push_back(m);
    }
    if (const double s = meanOfLast(m_recent, shortTermBlocks); s >= 0.0) {
        m_shortTerms.push_back(s);
    }
}

void LoudnessAnalysis::reset()
{
    *this = {};
}

double LoudnessAnalysis::momentary() const
{
    return lufsOrNone(meanOfLast(m_recent, momentaryBlocks));
}

double LoudnessAnalysis::shortTerm() const
{
    return lufsOrNone(meanOfLast(m_recent, shortTermBlocks));
}

double LoudnessAnalysis::integrated() const
{
    return lufsOrNone(gatedMean(m_gatingBlocks, relativeGate));
}

double LoudnessAnalysis::range() const
{
    const double gate = meanAbove(m_shortTerms, absoluteGate);
    if (gate < 0.0) {
        return none;
    }
    const double floor = std::max(absoluteGate, lufsFromMeanSquare(gate) + rangeGate);
    std::vector<double> values;
    for (const double s : m_shortTerms) {
        if (const double l = lufsFromMeanSquare(s); l > floor) {
            values.push_back(l);
        }
    }
    if (values.size() < 2) {
        return none;
    }
    std::ranges::sort(values);
    const auto at = [&values](double percentile) {
        const auto i = static_cast<std::size_t>(std::lround(percentile * static_cast<double>(values.size() - 1)));
        return values.at(i);
    };
    return at(0.95) - at(0.10);
}

double LoudnessAnalysis::truePeakDb() const
{
    return m_truePeak > 0.0 ? 20.0 * std::log10(m_truePeak) : none;
}

// --- LoudnessMeter -------------------------------------------------------------

LoudnessMeter::LoudnessMeter()
    : m_ring(4096) // 200 s of blocks: the main thread takes them ten times a second
{
}

void LoudnessMeter::setRate(int rate) noexcept
{
    if (rate <= 0 || rate == m_rate) {
        return;
    }
    m_rate = rate;
    m_weighting = KWeighting::forRate(rate);
    m_shelfState = {};
    m_passState = {};
    for (auto &d : m_detectors) {
        d.reset();
    }
    m_sum = 0.0;
    m_peak = 0.0;
    m_count = 0;
    m_blockLength = std::max(1, static_cast<int>(std::lround(rate * LoudnessAnalysis::blockSeconds)));
}

void LoudnessMeter::process(float *samples, std::uint32_t frames, std::uint32_t channelCount) noexcept
{
    const auto channels = std::min<std::size_t>(channelCount, maxEffectChannels);
    if (m_rate == 0 || channels == 0) {
        return;
    }
    const std::span<const float> in(samples, std::size_t{frames} * channelCount);
    for (std::uint32_t i = 0; i < frames; ++i) {
        double sum = 0.0;
        for (std::size_t c = 0; c < channels; ++c) {
            const float x = in[(std::size_t{i} * channelCount) + c];
            const double y = step(m_weighting.highPass, m_passState.at(c), step(m_weighting.shelf, m_shelfState.at(c), x));
            sum += y * y;
            m_peak = std::max(m_peak, static_cast<double>(std::max(m_detectors.at(c).process(x), std::abs(x))));
        }
        m_sum += sum;
        if (++m_count == m_blockLength) {
            const std::array<float, 2> block{static_cast<float>(m_sum / m_count), static_cast<float>(m_peak)};
            if (m_ring.space() >= block.size()) {
                m_ring.push(block.data(), block.size());
            }
            m_sum = 0.0;
            m_peak = 0.0;
            m_count = 0;
        }
    }
}

std::vector<LoudnessBlock> LoudnessMeter::take()
{
    std::vector<LoudnessBlock> out;
    std::array<float, 2> block{};
    while (m_ring.available() >= block.size()) {
        m_ring.pop(block.data(), block.size());
        out.push_back({.meanSquare = block[0], .truePeak = block[1]});
    }
    return out;
}

} // namespace pp

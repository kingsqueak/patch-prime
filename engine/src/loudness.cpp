// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "loudness.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pp {

namespace {

inline double step(const EqEffect::Coefficients &c, std::array<double, 2> &z, double x) noexcept
{
    const double y = (c.b0 * x) + z[0];
    z[0] = (c.b1 * x) - (c.a1 * y) + z[1];
    z[1] = (c.b2 * x) - (c.a2 * y);
    return y;
}

// Raises a meter to value; a compare-exchange, so a takeMeters() on the
// main thread in between can't lose it.
void raise(std::atomic<float> &meter, float value)
{
    float current = meter.load(std::memory_order_relaxed);
    while (value > current &&
           !meter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

KWeighting KWeighting::forRate(double sampleRate)
{
    KWeighting k;
    {
        // The head's acoustic effect: a high shelf of about +4 dB.
        constexpr double f0 = 1681.974450955533;
        constexpr double gain = 3.999843853973347;
        constexpr double q = 0.7071752369554196;
        const double kk = std::tan(std::numbers::pi * f0 / sampleRate);
        const double vh = std::pow(10.0, gain / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + (kk / q) + (kk * kk);
        k.shelf = {.b0 = (vh + (vb * kk / q) + (kk * kk)) / a0,
                   .b1 = 2.0 * ((kk * kk) - vh) / a0,
                   .b2 = (vh - (vb * kk / q) + (kk * kk)) / a0,
                   .a1 = 2.0 * ((kk * kk) - 1.0) / a0,
                   .a2 = (1.0 - (kk / q) + (kk * kk)) / a0};
    }
    {
        // The "RLB" high pass.
        constexpr double f0 = 38.13547087602444;
        constexpr double q = 0.5003270373238773;
        const double kk = std::tan(std::numbers::pi * f0 / sampleRate);
        const double a0 = 1.0 + (kk / q) + (kk * kk);
        k.highPass = {.b0 = 1.0,
                      .b1 = -2.0,
                      .b2 = 1.0,
                      .a1 = 2.0 * ((kk * kk) - 1.0) / a0,
                      .a2 = (1.0 - (kk / q) + (kk * kk)) / a0};
    }
    return k;
}

double lufsFromMeanSquare(double meanSquare)
{
    return -0.691 + (10.0 * std::log10(std::max(meanSquare, 1e-12)));
}

// --- LevelerEffect -----------------------------------------------------------------

void LevelerEffect::prepare(double sampleRate, int /*channels*/)
{
    m_rate = sampleRate;
    m_weighting = KWeighting::forRate(sampleRate);
    m_shelfState = {};
    m_passState = {};
    m_meanSquare = 0.0;
    m_window = 1.0 - std::exp(-1.0 / (1.0 * sampleRate)); // ~1 s
    m_fastSquare = 0.0;
    m_fastWindow = 1.0 - std::exp(-1.0 / (0.025 * sampleRate));
    m_gainDb = 0.0;
}

void LevelerEffect::setParam(const std::string &name, double value)
{
    if (name == "target") {
        m_target = static_cast<float>(std::clamp(value, -36.0, -6.0));
    } else if (name == "maxBoost") {
        m_maxBoost = static_cast<float>(std::clamp(value, 0.0, 24.0));
    } else if (name == "maxCut") {
        m_maxCut = static_cast<float>(std::clamp(value, 0.0, 24.0));
    } else if (name == "speed") {
        m_speed = static_cast<float>(std::clamp(value, 0.5, 30.0));
    } else if (name == "freeze") {
        m_freeze = static_cast<float>(std::clamp(value, -70.0, -20.0));
    }
}

std::size_t LevelerEffect::takeMeters(std::span<float> out)
{
    if (out.size() < 2) {
        return 0;
    }
    out[0] = m_gainMeter.load();
    out[1] = m_loudnessMeter.load();
    return 2;
}

void LevelerEffect::process(float *const *channels, int channelCount, int frames) noexcept
{
    const auto count = static_cast<std::size_t>(std::clamp(channelCount, 0, maxEffectChannels));
    const std::span<float *const> planes(channels, count);
    const double target = m_target.load(std::memory_order_relaxed);
    const double maxBoost = m_maxBoost.load(std::memory_order_relaxed);
    const double maxCut = m_maxCut.load(std::memory_order_relaxed);
    const double freeze = m_freeze.load(std::memory_order_relaxed);
    const double ride = 1.0 - std::exp(-1.0 / (m_speed.load(std::memory_order_relaxed) * m_rate));
    double loudness = -70.0;
    for (int i = 0; i < frames; ++i) {
        double sum = 0.0;
        for (std::size_t c = 0; c < count; ++c) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const double x = planes[c][i];
            const double y = step(m_weighting.highPass, m_passState.at(c),
                                  step(m_weighting.shelf, m_shelfState.at(c), x));
            sum += y * y;
        }
        m_fastSquare += (sum - m_fastSquare) * m_fastWindow;
        // On silence (by the fast reading) everything holds still: the gain,
        // and the slow reading too, so sound coming back isn't measured
        // against the silence and boosted.
        const bool active = lufsFromMeanSquare(m_fastSquare) > freeze;
        if (active) {
            m_meanSquare += (sum - m_meanSquare) * m_window;
        }
        loudness = lufsFromMeanSquare(m_meanSquare);
        if (active) {
            const double wanted = std::clamp(target - loudness, -maxCut, maxBoost);
            m_gainDb += (wanted - m_gainDb) * ride;
        }
        const auto gain = static_cast<float>(std::pow(10.0, m_gainDb / 20.0));
        for (float *plane : planes) {
            plane[i] *= gain; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }
    if (m_fastSquare < 1e-30) {
        m_fastSquare = 0.0;
    }
    for (std::size_t c = 0; c < count; ++c) {
        for (auto *state : {&m_shelfState.at(c), &m_passState.at(c)}) {
            for (double &v : *state) {
                if (std::abs(v) < 1e-18) {
                    v = 0.0;
                }
            }
        }
    }
    m_gainMeter.store(static_cast<float>(m_gainDb), std::memory_order_relaxed);
    m_loudnessMeter.store(static_cast<float>(loudness), std::memory_order_relaxed);
}

// --- LimiterEffect -------------------------------------------------------------------

void LimiterEffect::prepare(double sampleRate, int /*channels*/)
{
    m_rate = sampleRate;
    m_length = std::clamp(static_cast<int>(std::lround(lookaheadSeconds * sampleRate)) + 1, 1,
                          maxLookahead);
    resume();
}

void LimiterEffect::resume() noexcept
{
    m_at = 0;
    for (auto &line : m_delay) {
        line.fill(0.0F);
    }
    m_needed.fill(1.0);
    m_smoothed.fill(1.0);
    m_min = 1.0;
    m_minAge = 0;
    m_released = 1.0;
    m_sum = m_length;
}

void LimiterEffect::setParam(const std::string &name, double value)
{
    if (name == "ceiling") {
        m_ceiling = static_cast<float>(std::clamp(value, -24.0, 0.0));
    } else if (name == "release") {
        m_release = static_cast<float>(std::clamp(value, 5.0, 2000.0));
    }
}

std::size_t LimiterEffect::takeMeters(std::span<float> out)
{
    if (out.empty()) {
        return 0;
    }
    out[0] = m_meter.exchange(0.0F);
    return 1;
}

void LimiterEffect::process(float *const *channels, int channelCount, int frames) noexcept
{
    const auto count = static_cast<std::size_t>(std::clamp(channelCount, 0, maxEffectChannels));
    const std::span<float *const> planes(channels, count);
    const double ceiling = std::pow(10.0, m_ceiling.load(std::memory_order_relaxed) / 20.0);
    const auto ceilingF = static_cast<float>(ceiling);
    const double release =
        std::exp(-1.0 / (m_release.load(std::memory_order_relaxed) * 0.001 * m_rate));
    const auto length = static_cast<std::size_t>(m_length);
    double deepest = 1.0;

    // Each sample's needed gain is held as the minimum over the lookahead
    // window, released smoothly, then averaged over the same window. A peak
    // leaves the delay line (length - 1 samples late) exactly when that
    // average has come all the way down for it.
    for (int i = 0; i < frames; ++i) {
        const auto at = static_cast<std::size_t>(m_at);
        double peak = 0.0;
        for (std::size_t c = 0; c < count; ++c) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            peak = std::max(peak, static_cast<double>(std::abs(planes[c][i])));
        }
        const double need = peak > ceiling ? ceiling / peak : 1.0;
        m_needed.at(at) = need;
        if (need <= m_min) {
            m_min = need;
            m_minAge = 0;
        } else if (++m_minAge >= m_length) {
            // The minimum left the window: find the next one.
            m_min = 1.0;
            m_minAge = 0;
            for (std::size_t back = 0; back < length; ++back) {
                const double v = m_needed.at((at + length - back) % length);
                if (v < m_min) {
                    m_min = v;
                    m_minAge = static_cast<int>(back);
                }
            }
        }
        m_released = m_min < m_released ? m_min : m_min + ((m_released - m_min) * release);
        m_sum += m_released - m_smoothed.at(at);
        m_smoothed.at(at) = m_released;
        const double gain = m_sum / static_cast<double>(length);
        deepest = std::min(deepest, gain);

        const std::size_t out = (at + 1) % length; // written length - 1 samples ago
        for (std::size_t c = 0; c < count; ++c) {
            auto &line = m_delay.at(c);
            line.at(at) = planes[c][i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const float y = line.at(out) * static_cast<float>(gain);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            planes[c][i] = std::clamp(y, -ceilingF, ceilingF); // rounding can't get past
        }
        if (++m_at == m_length) {
            m_at = 0;
            // Recount the sum now and then, so rounding can't build up.
            m_sum = 0.0;
            for (std::size_t k = 0; k < length; ++k) {
                m_sum += m_smoothed.at(k);
            }
        }
    }
    raise(m_meter, static_cast<float>(-20.0 * std::log10(std::max(deepest, 1e-6))));
}

} // namespace pp

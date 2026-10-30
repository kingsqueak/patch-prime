// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace pp {

namespace {

float dbToGain(double db)
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

} // namespace

// --- GainEffect --------------------------------------------------------------

void GainEffect::prepare(double sampleRate, int /*channels*/)
{
    // About 20 ms to settle on a new level.
    m_smoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
    m_current = m_target.load();
}

void GainEffect::process(float *const *channels, int channelCount, int frames) noexcept
{
    const float target = m_target.load(std::memory_order_relaxed);
    const std::span<float *const> planes(channels, static_cast<std::size_t>(channelCount));
    float current = m_current;
    for (int i = 0; i < frames; ++i) {
        current += (target - current) * m_smoothing;
        for (float *plane : planes) {
            plane[i] *= current; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }
    m_current = current;
}

void GainEffect::setParam(const std::string &name, double value)
{
    if (name == "gainDb") {
        m_target.store(dbToGain(std::clamp(value, minDb, maxDb)));
    }
}

std::unique_ptr<Effect> makeEffect(const std::string &type)
{
    if (type == "gain") {
        return std::make_unique<GainEffect>();
    }
    return nullptr;
}

std::vector<std::string> effectTypes()
{
    return {"gain"};
}

std::string effectLabel(const std::string &type)
{
    if (type == "gain") {
        return "Gain";
    }
    return type;
}

std::vector<ParamInfo> effectParams(const std::string &type)
{
    if (type == "gain") {
        return {{.name = "gainDb",
                 .label = "Gain",
                 .unit = "dB",
                 .minimum = GainEffect::minDb,
                 .maximum = GainEffect::maxDb,
                 .initial = 0.0}};
    }
    return {};
}

// --- EffectChain --------------------------------------------------------------

EffectChain::EffectChain(std::vector<std::unique_ptr<Effect>> effects, double sampleRate,
                         int channels)
    : m_effects(std::move(effects)), m_channels(std::clamp(channels, 1, maxEffectChannels))
{
    m_slots.resize(m_effects.size());
    for (std::size_t i = 0; i < m_effects.size(); ++i) {
        m_effects[i]->prepare(sampleRate, m_channels);
        m_slots[i].mix = m_effects[i]->enabled.load() ? 1.0F : 0.0F;
    }
    m_dry.assign(static_cast<std::size_t>(m_channels),
                 std::vector<float>(static_cast<std::size_t>(maxEffectFrames)));
    m_step = static_cast<float>(1.0 / std::max(1.0, crossfadeSeconds * sampleRate));
}

void EffectChain::process(float *const *channels, int frames) noexcept
{
    frames = std::min(frames, maxEffectFrames);
    const auto count = static_cast<std::size_t>(m_channels);
    const std::span<float *const> planes(channels, count);
    for (std::size_t e = 0; e < m_effects.size(); ++e) {
        Effect &effect = *m_effects[e];
        float &mix = m_slots[e].mix;
        const float target = effect.enabled.load(std::memory_order_relaxed) ? 1.0F : 0.0F;
        if (mix == 0.0F && target == 0.0F) {
            continue; // bypassed: not processed at all
        }
        if (mix == 1.0F && target == 1.0F) {
            effect.process(channels, m_channels, frames);
            continue;
        }
        // Switching: process, then crossfade from the dry signal.
        for (std::size_t c = 0; c < count; ++c) {
            std::memcpy(m_dry[c].data(), planes[c],
                        static_cast<std::size_t>(frames) * sizeof(float));
        }
        effect.process(channels, m_channels, frames);
        float m = mix;
        for (int i = 0; i < frames; ++i) {
            m = target > m ? std::min(target, m + m_step) : std::max(target, m - m_step);
            for (std::size_t c = 0; c < count; ++c) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                float &wet = planes[c][i];
                const float dry = m_dry[c][static_cast<std::size_t>(i)];
                wet = dry + ((wet - dry) * m);
            }
        }
        mix = m;
    }
}

// --- Processor -----------------------------------------------------------------

Processor::~Processor()
{
    delete m_chain.exchange(nullptr);
}

void Processor::setChain(std::unique_ptr<EffectChain> chain)
{
    // Swap first, then look whether the realtime thread is inside process():
    // it marks that before reading the chain, so if it isn't, it can't be
    // holding the old one.
    EffectChain *old = m_chain.exchange(chain.release());
    if (old == nullptr) {
        return;
    }
    if (!m_inProcess.load()) {
        delete old;
        return;
    }
    m_retired.push_back({.chain = std::unique_ptr<EffectChain>(old), .cycle = m_cycle.load()});
}

void Processor::collect()
{
    const auto cycle = m_cycle.load();
    std::erase_if(m_retired, [cycle](const Retired &r) { return cycle > r.cycle; });
}

void Processor::process(const float *const *in, float *const *out, int channelCount,
                        int frames) noexcept
{
    m_inProcess.store(true);
    frames = std::min(frames, maxEffectFrames);
    const auto count = static_cast<std::size_t>(std::max(channelCount, 0));
    const std::span<const float *const> inputs(in, count);
    const std::span<float *const> outputs(out, count);
    for (std::size_t c = 0; c < count; ++c) {
        if (outputs[c] == nullptr) {
            continue;
        }
        if (inputs[c] == nullptr) {
            std::memset(outputs[c], 0, static_cast<std::size_t>(frames) * sizeof(float));
        } else if (inputs[c] != outputs[c]) {
            std::memcpy(outputs[c], inputs[c], static_cast<std::size_t>(frames) * sizeof(float));
        }
    }
    EffectChain *chain = m_chain.load();
    const bool complete = std::ranges::all_of(outputs, [](const float *p) { return p != nullptr; });
    if (chain != nullptr && complete && chain->channels() == channelCount) {
        chain->process(out, frames);
    }
    m_cycle.fetch_add(1);
    m_inProcess.store(false);
}

} // namespace pp

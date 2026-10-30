// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

// Built-in effects and the chain that runs them, for a strip's processing
// node. Everything here is plain C++ (no PipeWire), so it's unit tested.
//
// Threads: prepare() and parameter setters run on the main thread; process()
// runs on PipeWire's realtime thread and must not allocate, lock or throw.
// Parameters cross over as atomics.

constexpr int maxEffectChannels = 32;
constexpr int maxEffectFrames = 8192; // PipeWire's largest quantum

class Effect
{
public:
    Effect() = default;
    virtual ~Effect() = default;
    Effect(const Effect &) = delete;
    Effect &operator=(const Effect &) = delete;
    Effect(Effect &&) = delete;
    Effect &operator=(Effect &&) = delete;

    // The type name makeEffect() knows it by.
    [[nodiscard]] virtual std::string_view type() const = 0;
    // Main thread, before the effect runs: may allocate.
    virtual void prepare(double sampleRate, int channels) = 0;
    // Realtime: processes planar audio in place.
    virtual void process(float *const *channels, int channelCount, int frames) noexcept = 0;
    // Main thread: sets a parameter by name; unknown names are ignored.
    virtual void setParam(const std::string &name, double value) = 0;

    // On or bypassed. A change crossfades (see EffectChain).
    std::atomic<bool> enabled{true};
};

// A level trim: "gainDb" from -24 to +24 dB, smoothed so moving it doesn't
// zipper.
class GainEffect : public Effect
{
public:
    static constexpr double minDb = -24.0;
    static constexpr double maxDb = 24.0;

    [[nodiscard]] std::string_view type() const override { return "gain"; }
    void prepare(double sampleRate, int channels) override;
    void process(float *const *channels, int channelCount, int frames) noexcept override;
    void setParam(const std::string &name, double value) override;

private:
    std::atomic<float> m_target{1.0F}; // linear
    float m_current = 1.0F;            // realtime only
    float m_smoothing = 0.0F;          // per-sample coefficient
};

// The built-in effect types by name ("gain"); nullptr for an unknown type.
std::unique_ptr<Effect> makeEffect(const std::string &type);
// Every built-in type, in the order a menu lists them, and its display name.
std::vector<std::string> effectTypes();
std::string effectLabel(const std::string &type);
// Names and parameter ranges, for the UI and validation.
struct ParamInfo
{
    std::string name;
    std::string label;
    std::string unit; // "dB", "Hz", "ms", "" ...
    double minimum = 0.0;
    double maximum = 0.0;
    double initial = 0.0;
};
std::vector<ParamInfo> effectParams(const std::string &type);
// Effects one strip may have.
constexpr std::size_t maxEffectsPerStrip = 8;

// Effects in order. Each one's bypass crossfades between its dry input and
// its output over crossfadeSeconds, so switching never clicks; a fully
// bypassed effect isn't processed at all.
class EffectChain
{
public:
    static constexpr double crossfadeSeconds = 0.010;

    EffectChain(std::vector<std::unique_ptr<Effect>> effects, double sampleRate, int channels);

    [[nodiscard]] int channels() const { return m_channels; }
    [[nodiscard]] std::size_t size() const { return m_effects.size(); }
    [[nodiscard]] Effect &at(std::size_t index) { return *m_effects.at(index); }

    // Realtime.
    void process(float *const *channels, int frames) noexcept;

private:
    struct Slot
    {
        float mix = 1.0F; // 0 = bypassed, 1 = on; realtime only
    };

    std::vector<std::unique_ptr<Effect>> m_effects;
    std::vector<Slot> m_slots;
    std::vector<std::vector<float>> m_dry; // per channel, maxEffectFrames
    int m_channels = 0;
    float m_step = 1.0F; // crossfade change per sample
};

// What a strip's processing node runs each cycle: copies its input to its
// output and runs the current chain on it. The chain is swapped with
// setChain() from the main thread without locking the realtime thread; the
// old one is handed back by collect() once the realtime thread is past it.
class Processor
{
public:
    Processor() = default;
    ~Processor();
    Processor(const Processor &) = delete;
    Processor &operator=(const Processor &) = delete;
    Processor(Processor &&) = delete;
    Processor &operator=(Processor &&) = delete;

    // Main thread. nullptr: pass audio through unchanged.
    void setChain(std::unique_ptr<EffectChain> chain);
    // The current chain, for parameter changes (main thread; may be null).
    [[nodiscard]] EffectChain *chain() const { return m_chain.load(); }
    // Main thread: deletes chains the realtime thread can no longer be using.
    void collect();

    // Realtime: in and out are planar, channel by channel; in may equal out.
    void process(const float *const *in, float *const *out, int channelCount, int frames) noexcept;

private:
    std::atomic<EffectChain *> m_chain{nullptr};
    std::atomic<std::uint64_t> m_cycle{0}; // bumped at the end of every process()
    std::atomic<bool> m_inProcess{false};
    struct Retired
    {
        std::unique_ptr<EffectChain> chain;
        std::uint64_t cycle = 0; // m_cycle when it was swapped out
    };
    std::vector<Retired> m_retired;
};

} // namespace pp

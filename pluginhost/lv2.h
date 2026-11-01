// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "effects.h"
#include "eventring.h"
#include "hostedplugin.h"
#include "plugins.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace pp {

// LV2 through lilv: the installed plugins and what a host gives them
// (urid:map/unmap, options, bounded block length, log).
class Lv2World
{
public:
    Lv2World();
    ~Lv2World();
    Lv2World(const Lv2World &) = delete;
    Lv2World &operator=(const Lv2World &) = delete;
    Lv2World(Lv2World &&) = delete;
    Lv2World &operator=(Lv2World &&) = delete;

    // Every plugin installed, without loading any (it only reads their
    // descriptions).
    [[nodiscard]] std::vector<PluginInfo> scan() const;
    [[nodiscard]] const LilvPlugin *find(const std::string &uri) const;
    [[nodiscard]] PluginInfo describe(const LilvPlugin *plugin) const;

    [[nodiscard]] LilvWorld *world() const { return m_world; }
    // urid:map and unmap. A URI's string lives as long as the world (LV2
    // lets plugins keep what unmap returns).
    LV2_URID map(const char *uri);
    const char *unmap(LV2_URID urid);
    [[nodiscard]] LV2_URID_Map *mapFeature() { return &m_map; }
    [[nodiscard]] LV2_URID_Unmap *unmapFeature() { return &m_unmap; }
    // Features for an instance (or its UI): options is its own options
    // feature (the rate and block sizes), which must outlive it.
    [[nodiscard]] std::vector<const LV2_Feature *> features(const LV2_Feature *options);
    // Why a plugin can't run here, or "" when it can.
    [[nodiscard]] std::string problem(const LilvPlugin *plugin) const;

    // Node URIs used when describing ports.
    struct Uris
    {
        LilvNode *audio = nullptr;
        LilvNode *control = nullptr;
        LilvNode *atom = nullptr;
        LilvNode *cv = nullptr;
        LilvNode *input = nullptr;
        LilvNode *output = nullptr;
        LilvNode *sideChain = nullptr;
        LilvNode *toggled = nullptr;
        LilvNode *integer = nullptr;
        LilvNode *enumeration = nullptr;
        LilvNode *logarithmic = nullptr;
        LilvNode *notOnGui = nullptr;
        LilvNode *unit = nullptr;
        LilvNode *unitSymbol = nullptr;
        LilvNode *author = nullptr;
    };
    [[nodiscard]] const Uris &uris() const { return m_uris; }

private:
    LilvWorld *m_world;
    Uris m_uris;
    std::mutex m_mapLock;
    std::map<std::string, LV2_URID> m_ids;
    std::deque<std::string> m_names; // by id - 1; never moved
    LV2_URID_Map m_map{};
    LV2_URID_Unmap m_unmap{};
    LV2_Log_Log m_log{};
    LV2_Feature m_mapFeature{};
    LV2_Feature m_unmapFeature{};
    LV2_Feature m_logFeature{};
    LV2_Feature m_boundedFeature{};
};

// An LV2 plugin as an effect in a chain (see EffectChain). It processes
// out of place: the strip's channels are copied to the plugin's audio
// inputs and back from its outputs. A plugin with one input and one output
// runs once per channel on a stereo strip; otherwise channels are spread
// over its ports (averaged when it has fewer). Sidechain inputs get silence.
// Parameters are its control input ports, by symbol.
class Lv2Effect : public HostedPlugin
{
public:
    Lv2Effect(Lv2World &world, const LilvPlugin *plugin);
    ~Lv2Effect() override;
    Lv2Effect(const Lv2Effect &) = delete;
    Lv2Effect &operator=(const Lv2Effect &) = delete;
    Lv2Effect(Lv2Effect &&) = delete;
    Lv2Effect &operator=(Lv2Effect &&) = delete;

    [[nodiscard]] std::string_view type() const override { return m_type; }
    // Instantiates and activates the plugin (see error()).
    void prepare(double sampleRate, int channels) override;
    void process(float *const *channels, int channelCount, int frames) noexcept override;
    void setParam(const std::string &name, double value) override;

    [[nodiscard]] const std::string &error() const override { return m_error; }
    std::unique_ptr<PluginWindow>
    openWindow(const std::string &title, std::function<void(const std::string &, double)> onParam) override;

    // --- For its UI (see Lv2Ui), on the main thread ---
    [[nodiscard]] const LilvPlugin *plugin() const { return m_plugin; }
    [[nodiscard]] float sampleRate() const { return m_rate; }
    // The (first) instance, for UIs that reach into it (instance-access).
    [[nodiscard]] LV2_Handle handle() const;
    [[nodiscard]] const LV2_Descriptor *descriptor() const;
    // Control ports, inputs and outputs, by index.
    [[nodiscard]] std::vector<std::uint32_t> controlPorts() const;
    // A control port's value: an input's as set, an output's as the plugin
    // last wrote it (a meter, say).
    [[nodiscard]] float controlValue(std::uint32_t index) const;
    // Sets a control input from the UI; returns its symbol, or "" when the
    // port isn't a control input.
    std::string setControl(std::uint32_t index, float value);
    // Events from the UI to an atom input (instance 0), delivered before the
    // next block; false when the port isn't one, or there's no room.
    bool sendAtom(std::uint32_t index, std::span<const std::uint8_t> atom);
    // Events the plugin wrote to its atom outputs since the last call, as
    // fn(port index, atom); collected only while forwarding is on.
    void setForwarding(bool on) { m_forwarding.store(on, std::memory_order_relaxed); }
    void takeEvents(const std::function<void(std::uint32_t, const LV2_Atom &)> &fn);

    // The plugin's own state (state:interface), apart from its ports: what
    // a UI may have changed (a loaded file, say). Saving runs alongside the
    // audio; restoring happens in prepare(), before the plugin starts.
    [[nodiscard]] bool hasState() const;
    [[nodiscard]] std::string saveState() const override;
    void setInitialState(std::string state) override { m_initialState = std::move(state); }

private:
    enum class PortKind : std::uint8_t
    {
        AudioIn,
        AudioOut,
        SideIn, // a sidechain audio input: fed silence
        ControlIn,
        ControlOut,
        AtomIn,
        AtomOut,
        Other, // CV, or a type we don't know: a scratch buffer
    };
    struct Port
    {
        std::uint32_t index = 0;
        PortKind kind = PortKind::Other;
        std::string symbol;
        bool input = false;
        float initial = 0.0F;
        float minimum = 0.0F;
        float maximum = 1.0F;
    };
    struct Instance
    {
        LilvInstance *instance = nullptr;
        std::vector<float> controls; // by port index
        std::vector<std::vector<float>> audioIn;  // per audio input port
        std::vector<std::vector<float>> audioOut; // per audio output port
        std::vector<std::vector<std::uint8_t>> atomIn;
        std::vector<std::vector<std::uint8_t>> atomOut;
        // A buffer of its own for every other port (sidechains, CV): no two
        // ports share one. The inputs among them are cleared every block.
        std::vector<std::vector<float>> other;
        std::vector<float *> clearEach;
    };
    void free();

    Lv2World &m_world;
    const LilvPlugin *m_plugin;
    std::string m_type;
    std::vector<Port> m_ports;
    std::vector<std::uint32_t> m_audioIn;  // port indexes, in order
    std::vector<std::uint32_t> m_audioOut;
    std::vector<int> m_atomSlot; // by port index: its place in atomIn/atomOut, or -1
    // Parameters by control port index, set from the main thread; copied
    // into each instance's controls every block.
    std::map<std::string, std::uint32_t> m_bySymbol;
    std::unique_ptr<std::atomic<float>[]> m_targets; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): atomics can't live in a vector
    std::vector<Instance> m_instances;
    int m_channels = 0;
    std::int32_t m_blockMax = maxEffectFrames;
    std::int32_t m_blockMin = 1;
    float m_rate = 48000.0F;
    std::vector<LV2_Options_Option> m_options;
    LV2_Feature m_optionsFeature{};
    std::int32_t m_sequenceSize = 0;
    std::int32_t m_blockNominal = 1024;
    LV2_URID m_sequence = 0;
    LV2_URID m_chunk = 0;
    std::string m_error;
    std::string m_initialState;
    // Atom events between the UI and the realtime thread, and scratch space
    // for one event on each side.
    EventRing m_toPlugin;
    EventRing m_toUi;
    std::vector<std::uint8_t> m_rtScratch;
    std::vector<std::uint8_t> m_eventScratch; // an event being appended (realtime)
    std::vector<std::uint8_t> m_uiScratch;
    std::atomic<bool> m_forwarding{false};
};

} // namespace pp

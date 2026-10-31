// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "hostedplugin.h"
#include "plugins.h"
#include "x11window.h"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class QObject;
class QSocketNotifier;
class QTimer;

namespace pp {

// CLAP plugins: where they're installed, and what each .clap file holds.

// The .clap files in the search path ($CLAP_PATH, ~/.clap, /usr/lib/clap,
// /usr/local/lib/clap), recursively; only $PATCHPRIME_CLAP_PATH when it's
// set (the tests' own plugin).
std::vector<std::string> clapFiles();
// The plugins in one .clap file. Each is created and initialised (not
// activated) to read its parameters and ports, so a crashing plugin crashes
// the caller: run it in a process of its own (patchprime-pluginhost
// --scan-clap <file>).
std::vector<PluginInfo> scanClapFile(const std::string &path);
// A category for CLAP's feature names ("compressor" -> "Compressor").
std::string clapCategory(const std::vector<std::string> &features);

class ClapWindow;

// A CLAP plugin as an effect in a chain (see EffectChain). Like Lv2Effect:
// the strip's channels go to its main audio input and come back from its
// main output (one instance per channel for a mono plugin on a stereo
// strip; other ports get silence). Parameters are named by their CLAP id,
// in decimal, and reach the plugin as parameter events; those the plugin
// changes itself (in its editor) come out as events too, for the engine to
// save (takeParamChanges).
class ClapEffect : public HostedPlugin
{
public:
    // Loads plugin id from the .clap file at path (see error()).
    ClapEffect(const std::string &path, const std::string &id);
    ~ClapEffect() override;
    ClapEffect(const ClapEffect &) = delete;
    ClapEffect &operator=(const ClapEffect &) = delete;
    ClapEffect(ClapEffect &&) = delete;
    ClapEffect &operator=(ClapEffect &&) = delete;

    [[nodiscard]] std::string_view type() const override { return m_type; }
    void prepare(double sampleRate, int channels) override;
    void process(float *const *channels, int channelCount, int frames) noexcept override;
    void setParam(const std::string &name, double value) override;

    [[nodiscard]] const std::string &error() const override { return m_error; }
    [[nodiscard]] std::string saveState() const override;
    void setInitialState(std::string state) override { m_initialState = std::move(state); }
    bool takeStateDirty() override { return m_stateDirty.exchange(false); }
    std::unique_ptr<PluginWindow> openWindow(const std::string &title) override;
    void takeParamChanges(const std::function<void(const std::string &, double)> &fn) override;

private:
    friend class ClapWindow;

    struct Param
    {
        clap_id id = 0;
        void *cookie = nullptr;
        double minimum = 0.0;
        double maximum = 1.0;
        double initial = 0.0;
    };
    // One plugin instance with what the host gives it.
    struct Instance
    {
        ClapEffect *effect = nullptr;
        clap_host_t host{};
        const clap_plugin_t *plugin = nullptr;
        bool active = false;
        bool processing = false; // start_processing called (realtime)
        std::vector<double> sent; // per param: what it was last sent (realtime)
        // This block's events in (parameter values) and the lists' vtables.
        std::vector<clap_event_param_value_t> events;
        std::uint32_t eventCount = 0;
        clap_input_events_t in{};
        clap_output_events_t out{};
        // Audio: per port, its channels' buffers and pointers.
        std::vector<std::vector<std::vector<float>>> inBuffers;
        std::vector<std::vector<std::vector<float>>> outBuffers;
        std::vector<std::vector<float *>> inPointers;
        std::vector<std::vector<float *>> outPointers;
        std::vector<clap_audio_buffer_t> inPorts;
        std::vector<clap_audio_buffer_t> outPorts;
    };
    struct PortLayout
    {
        std::uint32_t channels = 0;
        bool main = false;
    };

    std::unique_ptr<Instance> makeInstance();
    static void destroyInstance(Instance &instance);
    void readPorts(const clap_plugin_t *plugin);
    void loadState(const clap_plugin_t *plugin) const;

    // The host's side (clap_host_t and its extensions).
    static const void *hostExtension(const clap_host_t *host, const char *id);
    static void requestRestart(const clap_host_t *host);
    static void requestProcess(const clap_host_t *host);
    static void requestCallback(const clap_host_t *host);
    static ClapEffect &of(const clap_host_t *host);

    std::string m_type;
    std::string m_error;
    std::string m_initialState;
    const clap_plugin_entry_t *m_entry = nullptr;
    const clap_plugin_factory_t *m_factory = nullptr;
    std::string m_id;
    std::vector<std::unique_ptr<Instance>> m_instances; // the first one also has the editor
    std::vector<Param> m_params;
    std::map<clap_id, std::size_t> m_paramIndex;
    std::unique_ptr<std::atomic<double>[]> m_targets;   // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): atomics can't live in a vector
    std::unique_ptr<std::atomic<bool>[]> m_changed;     // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): by the plugin, for the engine
    std::vector<PortLayout> m_inLayout;
    std::vector<PortLayout> m_outLayout;
    std::vector<float> m_silence;
    // The parameters' values for this block, the same for every instance
    // (realtime): what an instance changes shows from the next block on.
    std::vector<double> m_block;
    std::int64_t m_steadyTime = 0;
    std::atomic<bool> m_stateDirty{false};
    std::atomic<bool> m_callbackQueued{false};
    std::thread::id m_mainThread;
    ClapWindow *m_window = nullptr;
    // Timers and file descriptors the plugin asked for (its editor's), run
    // by Qt on the main thread.
    std::map<clap_id, QTimer *> m_timers;
    clap_id m_nextTimer = 1;
    std::map<int, QSocketNotifier *> m_fds;
    QObject *m_context = nullptr; // owns the timers and notifiers
};

// A CLAP plugin's own editor (clap.gui, X11, embedded) in an X11Window.
class ClapWindow : public PluginWindow
{
public:
    ClapWindow(ClapEffect &effect, const std::string &title);
    ~ClapWindow() override;
    ClapWindow(const ClapWindow &) = delete;
    ClapWindow &operator=(const ClapWindow &) = delete;
    ClapWindow(ClapWindow &&) = delete;
    ClapWindow &operator=(ClapWindow &&) = delete;

    [[nodiscard]] const std::string &error() const override { return m_error; }
    bool idle() override;
    void raise() override { m_window.raise(); }

private:
    friend class ClapEffect;
    void resizeTo(std::uint32_t width, std::uint32_t height);

    ClapEffect &m_effect;
    std::string m_error;
    X11Window m_window;
    const clap_plugin_t *m_plugin = nullptr;
    const clap_plugin_gui_t *m_gui = nullptr;
    bool m_created = false;
    bool m_closed = false;
};

} // namespace pp

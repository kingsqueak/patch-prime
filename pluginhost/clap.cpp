// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "clap.h"

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <print>
#include <set>
#include <span>

// CLAP's ids (CLAP_EXT_PARAMS, CLAP_PLUGIN_FACTORY_ID, ...) are char arrays,
// used as the C strings they are.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

namespace pp {

namespace {

thread_local bool t_audioThread = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set by the realtime thread for thread-check

// A .clap file's entry point, loaded once and kept for the process's life
// (unloading plugin libraries isn't safe in general).
const clap_plugin_entry_t *loadEntry(const std::string &path, std::string &error)
{
    static std::map<std::string, const clap_plugin_entry_t *> loaded;
    if (const auto it = loaded.find(path); it != loaded.end()) {
        return it->second;
    }
    void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        error = std::string("it couldn't be loaded: ") + dlerror();
        return nullptr;
    }
    const auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(library, "clap_entry"));
    if (entry == nullptr || !clap_version_is_compatible(entry->clap_version)) {
        error = "it isn't a CLAP plugin this version of CLAP can load";
        return nullptr;
    }
    if (!entry->init(path.c_str())) {
        error = "its library didn't start";
        return nullptr;
    }
    loaded[path] = entry;
    return entry;
}

// The host the scan creates plugins with: no extensions, nothing to do.
const clap_host_t &scanHost()
{
    static const clap_host_t host{
        .clap_version = CLAP_VERSION,
        .host_data = nullptr,
        .name = "Patch Prime",
        .vendor = "Patch Prime",
        .url = "https://github.com/kingsqueak/patch-prime",
        .version = "1",
        .get_extension = [](const clap_host_t *, const char *) -> const void * { return nullptr; },
        .request_restart = [](const clap_host_t *) {},
        .request_process = [](const clap_host_t *) {},
        .request_callback = [](const clap_host_t *) {},
    };
    return host;
}

std::string clapType(std::string_view id)
{
    return std::string(clapTypePrefix) + std::string(id);
}

// A value as the plugin writes it ("-6.0 dB").
std::string valueText(const clap_plugin_t *plugin, const clap_plugin_params_t *params, clap_id id,
                      double value)
{
    std::array<char, 128> text{};
    if (params->value_to_text == nullptr ||
        !params->value_to_text(plugin, id, value, text.data(), static_cast<std::uint32_t>(text.size()))) {
        return {};
    }
    return {text.data(), strnlen(text.data(), text.size())}; // a full buffer has no end mark
}

// The unit in a value's text: what follows the number ("dB" in "-6.0 dB").
std::string unitOf(const std::string &text)
{
    std::size_t i = 0;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0 ||
                               std::strchr(" +-.,", text[i]) != nullptr)) {
        ++i;
    }
    if (i == 0) {
        return {};
    }
    std::string unit = text.substr(i);
    while (!unit.empty() && unit.back() == ' ') {
        unit.pop_back();
    }
    const bool plain = std::ranges::none_of(unit, [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    return plain && unit.size() <= 5 ? unit : std::string();
}

// What the engine needs to know of a parameter (see ParamInfo).
ParamInfo describeParam(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                        const clap_param_info_t &p)
{
    ParamInfo info;
    info.name = std::to_string(p.id);
    info.label = std::string(p.name, strnlen(p.name, CLAP_NAME_SIZE));
    info.minimum = std::min(p.min_value, p.max_value);
    info.maximum = std::max(p.min_value, p.max_value);
    info.initial = std::clamp(p.default_value, info.minimum, info.maximum);
    info.unit = unitOf(valueText(plugin, params, p.id, info.initial));
    const double span = info.maximum - info.minimum;
    if ((p.flags & CLAP_PARAM_IS_STEPPED) != 0 && info.minimum == 0.0 && span >= 1.0 && span <= 31.0) {
        if (span == 1.0) {
            info.kind = ParamKind::Toggle;
        } else {
            info.kind = ParamKind::Choice;
            for (int v = 0; v <= static_cast<int>(span); ++v) {
                auto label = valueText(plugin, params, p.id, v);
                info.options.push_back(label.empty() ? std::to_string(v) : label);
            }
        }
        info.unit.clear();
    } else if (info.minimum > 0.0 && (info.unit == "Hz" || info.unit == "kHz")) {
        info.kind = ParamKind::Log;
    }
    return info;
}

// Parameters a user sets: not hidden, not read-only.
bool settable(const clap_param_info_t &p)
{
    return (p.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) == 0;
}

// The channel counts of a plugin's main audio ports (0 when it has none).
std::pair<std::uint32_t, std::uint32_t> mainChannels(const clap_plugin_t *plugin)
{
    const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (ports == nullptr) {
        return {0, 0};
    }
    std::pair<std::uint32_t, std::uint32_t> out{0, 0};
    for (const bool input : {true, false}) {
        for (std::uint32_t i = 0; i < ports->count(plugin, input); ++i) {
            clap_audio_port_info_t info{};
            if (ports->get(plugin, i, input, &info) && (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0) {
                (input ? out.first : out.second) = info.channel_count;
                break;
            }
        }
    }
    return out;
}

// Copies the strip's channels into a port's channels, or back: each
// destination channel gets the matching source channel, or the average of
// the source channels that fall on it when there are fewer destinations.
void spread(std::span<const float *const> from, std::span<float *const> to, std::size_t n)
{
    if (from.empty() || to.empty()) {
        return;
    }
    for (std::size_t d = 0; d < to.size(); ++d) {
        float *dest = to[d];
        if (to.size() >= from.size()) {
            std::memcpy(dest, from[d % from.size()], n * sizeof(float));
            continue;
        }
        std::fill_n(dest, n, 0.0F);
        int summed = 0;
        for (std::size_t s = d; s < from.size(); s += to.size()) {
            const float *src = from[s];
            for (std::size_t i = 0; i < n; ++i) {
                dest[i] += src[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            }
            ++summed;
        }
        const float scale = 1.0F / static_cast<float>(std::max(summed, 1));
        for (std::size_t i = 0; i < n; ++i) {
            dest[i] *= scale; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }
}

} // namespace

std::vector<std::string> clapFiles()
{
    std::vector<std::filesystem::path> dirs;
    // The tests look only in their own directory.
    const char *only = std::getenv("PATCHPRIME_CLAP_PATH"); // NOLINT(concurrency-mt-unsafe): read at start-up
    const char *env = only != nullptr ? only : std::getenv("CLAP_PATH"); // NOLINT(concurrency-mt-unsafe): read at start-up
    if (env != nullptr) {
        std::string_view rest(env);
        while (!rest.empty()) {
            const auto colon = rest.find(':');
            if (const auto dir = rest.substr(0, colon); !dir.empty()) {
                dirs.emplace_back(dir);
            }
            rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        }
    }
    if (only == nullptr) {
        if (const char *home = std::getenv("HOME"); home != nullptr) { // NOLINT(concurrency-mt-unsafe): read at start-up
            dirs.emplace_back(std::filesystem::path(home) / ".clap");
        }
        dirs.emplace_back("/usr/lib/clap");
        dirs.emplace_back("/usr/local/lib/clap");
    }

    std::set<std::string> seen;
    std::vector<std::string> files;
    for (const auto &dir : dirs) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 dir, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->path().extension() != ".clap" || !it->is_regular_file(error)) {
                continue;
            }
            auto path = std::filesystem::weakly_canonical(it->path(), error).string();
            if (seen.insert(path).second) {
                files.push_back(std::move(path));
            }
        }
    }
    return files;
}

std::string clapCategory(const std::vector<std::string> &features)
{
    // Most specific first.
    static const std::vector<std::pair<std::string, std::string>> names{
        {"compressor", "Compressor"},       {"limiter", "Limiter"},
        {"gate", "Gate"},                   {"expander", "Expander"},
        {"transient-shaper", "Dynamics"},   {"equalizer", "Parametric EQ"},
        {"reverb", "Reverb"},               {"delay", "Delay"},
        {"chorus", "Chorus"},               {"flanger", "Flanger"},
        {"phaser", "Phaser"},               {"distortion", "Distortion"},
        {"filter", "Filter"},               {"pitch-shifter", "Pitch"},
        {"pitch-correction", "Pitch"},      {"restoration", "Restoration"},
        {"analyzer", "Analyser"},           {"mixing", "Mixer"},
        {"mastering", "Mastering"},         {"utility", "Utility"},
        {"instrument", "Instrument"},       {"synthesizer", "Instrument"},
    };
    for (const auto &[feature, name] : names) {
        if (std::ranges::find(features, feature) != features.end()) {
            return name;
        }
    }
    return "Other";
}

std::vector<PluginInfo> scanClapFile(const std::string &path)
{
    std::vector<PluginInfo> out;
    std::string error;
    const auto *entry = loadEntry(path, error);
    if (entry == nullptr) {
        std::println(stderr, "patchprime-pluginhost: {}: {}", path, error);
        return out;
    }
    const auto *factory =
        static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr) {
        return out;
    }
    const auto count = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, i);
        if (d == nullptr || d->id == nullptr) {
            continue;
        }
        PluginInfo p;
        p.type = clapType(d->id);
        p.name = d->name != nullptr ? d->name : d->id;
        p.author = d->vendor != nullptr ? d->vendor : "";
        p.binary = path;
        std::vector<std::string> features;
        for (const char *const *f = d->features; f != nullptr && *f != nullptr; ++f) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            features.emplace_back(*f);
        }
        p.category = clapCategory(features);

        const clap_plugin_t *plugin = factory->create_plugin(factory, &scanHost(), d->id);
        if (plugin == nullptr || !plugin->init(plugin)) {
            p.problem = "it couldn't be started";
            if (plugin != nullptr) {
                plugin->destroy(plugin);
            }
            out.push_back(std::move(p));
            continue;
        }
        if (const auto *params = static_cast<const clap_plugin_params_t *>(
                plugin->get_extension(plugin, CLAP_EXT_PARAMS))) {
            for (std::uint32_t k = 0; k < params->count(plugin); ++k) {
                clap_param_info_t info{};
                if (params->get_info(plugin, k, &info) && settable(info)) {
                    p.params.push_back(describeParam(plugin, params, info));
                }
            }
        }
        const auto [in, out_] = mainChannels(plugin);
        p.audioIn = static_cast<int>(in);
        p.audioOut = static_cast<int>(out_);
        if (const auto *gui = static_cast<const clap_plugin_gui_t *>(
                plugin->get_extension(plugin, CLAP_EXT_GUI))) {
            p.hasUi = gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false);
        }
        plugin->destroy(plugin);
        if (p.audioIn == 0 || p.audioOut == 0) {
            p.problem = "it doesn't process audio (an instrument, or an analyser)";
        }
        out.push_back(std::move(p));
    }
    return out;
}

// --- ClapEffect ---------------------------------------------------------------

ClapEffect::ClapEffect(const std::string &path, const std::string &id)
    : m_type(clapType(id)), m_id(id), m_mainThread(std::this_thread::get_id()),
      m_context(new QObject)
{
    m_entry = loadEntry(path, m_error); // NOLINT(cppcoreguidelines-prefer-member-initializer): reports into m_error
    if (m_entry == nullptr) {
        return;
    }
    m_factory = static_cast<const clap_plugin_factory_t *>(m_entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (m_factory == nullptr) {
        m_error = "it has no plugins";
        return;
    }
    auto first = makeInstance();
    if (!first) {
        m_error = "the plugin couldn't be started";
        return;
    }
    const clap_plugin_t *plugin = first->plugin;
    if (const auto *params = static_cast<const clap_plugin_params_t *>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS))) {
        for (std::uint32_t k = 0; k < params->count(plugin); ++k) {
            clap_param_info_t info{};
            if (!params->get_info(plugin, k, &info) || !settable(info)) {
                continue;
            }
            m_paramIndex[info.id] = m_params.size();
            m_params.push_back({.id = info.id,
                                .cookie = info.cookie,
                                .minimum = std::min(info.min_value, info.max_value),
                                .maximum = std::max(info.min_value, info.max_value),
                                .initial = info.default_value});
        }
    }
    m_targets = std::make_unique<std::atomic<double>[]>(m_params.size()); // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    m_changed = std::make_unique<std::atomic<bool>[]>(m_params.size());   // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    for (std::size_t i = 0; i < m_params.size(); ++i) {
        m_targets[i].store(std::clamp(m_params[i].initial, m_params[i].minimum, m_params[i].maximum));
        m_changed[i].store(false);
    }
    readPorts(plugin);
    const bool hasMain = std::ranges::any_of(m_inLayout, &PortLayout::main) &&
                         std::ranges::any_of(m_outLayout, &PortLayout::main);
    if (!hasMain) {
        m_error = "it has no main audio input and output";
    }
    m_instances.push_back(std::move(first));
}

ClapEffect::~ClapEffect()
{
    delete m_context; // timers and notifiers first: they call into the plugin
    m_context = nullptr;
    for (auto &instance : m_instances) {
        destroyInstance(*instance);
    }
}

std::unique_ptr<ClapEffect::Instance> ClapEffect::makeInstance()
{
    auto inst = std::make_unique<Instance>();
    inst->effect = this;
    inst->host = {
        .clap_version = CLAP_VERSION,
        .host_data = inst.get(),
        .name = "Patch Prime",
        .vendor = "Patch Prime",
        .url = "https://github.com/kingsqueak/patch-prime",
        .version = "1",
        .get_extension = &ClapEffect::hostExtension,
        .request_restart = &ClapEffect::requestRestart,
        .request_process = &ClapEffect::requestProcess,
        .request_callback = &ClapEffect::requestCallback,
    };
    inst->plugin = m_factory->create_plugin(m_factory, &inst->host, m_id.c_str());
    if (inst->plugin == nullptr) {
        return nullptr;
    }
    if (!inst->plugin->init(inst->plugin)) {
        inst->plugin->destroy(inst->plugin);
        return nullptr;
    }
    return inst;
}

void ClapEffect::destroyInstance(Instance &instance)
{
    if (instance.plugin == nullptr) {
        return;
    }
    // The realtime thread has stopped by now (the node is gone). It normally
    // stopped processing there too (stopProcessingSoon); if it couldn't (the
    // graph wasn't running), here.
    if (instance.processing) {
        instance.plugin->stop_processing(instance.plugin);
    }
    if (instance.active) {
        instance.plugin->deactivate(instance.plugin);
    }
    instance.plugin->destroy(instance.plugin);
    instance.plugin = nullptr;
}

void ClapEffect::readPorts(const clap_plugin_t *plugin)
{
    const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (ports == nullptr) {
        return;
    }
    for (const bool input : {true, false}) {
        auto &layout = input ? m_inLayout : m_outLayout;
        layout.clear();
        bool mainSeen = false;
        for (std::uint32_t i = 0; i < ports->count(plugin, input); ++i) {
            clap_audio_port_info_t info{};
            // Every port, as the plugin counts them (it indexes the buffers
            // the same way), and none too wide.
            if (!ports->get(plugin, i, input, &info) || info.channel_count > maxEffectChannels) {
                m_error = "its audio ports couldn't be read";
                layout.clear();
                return;
            }
            const bool main = !mainSeen && (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
            mainSeen = mainSeen || main;
            layout.push_back({.channels = info.channel_count, .main = main});
        }
    }
}

void ClapEffect::loadState(const clap_plugin_t *plugin) const
{
    if (m_initialState.empty()) {
        return;
    }
    const auto *state =
        static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (state == nullptr) {
        return;
    }
    struct Reader
    {
        QByteArray data;
        qsizetype at = 0;
    } reader{.data = QByteArray::fromBase64(QByteArray::fromStdString(m_initialState)), .at = 0};
    const clap_istream_t stream{
        .ctx = &reader,
        .read = [](const clap_istream_t *s, void *buffer, std::uint64_t size) -> std::int64_t {
            auto *r = static_cast<Reader *>(s->ctx);
            const auto left = static_cast<std::uint64_t>(r->data.size() - r->at);
            const auto n = std::min(size, left);
            std::memcpy(buffer, r->data.constData() + r->at, static_cast<std::size_t>(n)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            r->at += static_cast<qsizetype>(n);
            return static_cast<std::int64_t>(n);
        },
    };
    if (!state->load(plugin, &stream)) {
        // It carries on as it was: better than not at all.
        std::println(stderr, "patchprime-pluginhost: the plugin couldn't take its saved state");
    }
}

void ClapEffect::prepare(double sampleRate, int channels)
{
    if (!m_error.empty() || m_instances.empty()) {
        return;
    }
    std::uint32_t mainIn = 0;
    std::uint32_t mainOut = 0;
    for (const auto &l : m_inLayout) {
        mainIn = l.main ? l.channels : mainIn;
    }
    for (const auto &l : m_outLayout) {
        mainOut = l.main ? l.channels : mainOut;
    }
    // A mono plugin on a stereo strip: one per channel.
    const bool perChannel = mainIn == 1 && mainOut == 1 && channels > 1;
    const std::size_t count = perChannel ? static_cast<std::size_t>(channels) : 1;
    while (m_instances.size() < count) {
        auto instance = makeInstance();
        if (!instance) {
            m_error = "the plugin couldn't be started";
            return;
        }
        m_instances.push_back(std::move(instance));
    }

    // Its state first: that may change its ports, which are read again
    // before any buffer is made.
    for (auto &inst : m_instances) {
        loadState(inst->plugin);
    }
    readPorts(m_instances.front()->plugin);
    if (!m_error.empty()) {
        return;
    }

    const auto frames = static_cast<std::size_t>(maxEffectFrames);
    m_silence.assign(frames, 0.0F);
    m_block.assign(m_params.size(), 0.0);
    for (auto &inst : m_instances) {
        inst->params = static_cast<const clap_plugin_params_t *>(
            inst->plugin->get_extension(inst->plugin, CLAP_EXT_PARAMS));
        inst->sent.assign(m_params.size(), std::numeric_limits<double>::quiet_NaN());
        inst->events.assign(m_params.size(), clap_event_param_value_t{});
        inst->eventCount = 0;
        inst->in = {
            .ctx = inst.get(),
            .size = [](const clap_input_events_t *list) -> std::uint32_t {
                return static_cast<const Instance *>(list->ctx)->eventCount;
            },
            .get = [](const clap_input_events_t *list, std::uint32_t index) -> const clap_event_header_t * {
                const auto *i = static_cast<const Instance *>(list->ctx);
                return index < i->eventCount ? &i->events[index].header : nullptr;
            },
        };
        inst->out = {
            .ctx = inst.get(),
            .try_push = [](const clap_output_events_t *list, const clap_event_header_t *event) -> bool {
                auto *i = static_cast<Instance *>(list->ctx);
                if (event->space_id != CLAP_CORE_EVENT_SPACE_ID || event->type != CLAP_EVENT_PARAM_VALUE) {
                    return true; // gestures and the like: nothing to do
                }
                const auto *value = reinterpret_cast<const clap_event_param_value_t *>(event); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                ClapEffect &self = *i->effect;
                const auto at = self.m_paramIndex.find(value->param_id);
                if (at == self.m_paramIndex.end() || i != self.m_instances.front().get()) {
                    return true;
                }
                if (!std::isfinite(value->value)) {
                    return true; // nothing to keep
                }
                // The plugin changed it (in its editor): keep it, and tell the engine.
                self.m_targets[at->second].store(value->value);
                i->sent[at->second] = value->value;
                self.m_changed[at->second].store(true);
                return true;
            },
        };
        // Audio buffers for every port; the main ones carry the strip.
        const auto setUp = [&](const std::vector<PortLayout> &layout, auto &buffers, auto &pointers,
                               auto &ports, bool input) {
            buffers.assign(layout.size(), {});
            pointers.assign(layout.size(), {});
            ports.assign(layout.size(), clap_audio_buffer_t{});
            for (std::size_t p = 0; p < layout.size(); ++p) {
                for (std::uint32_t c = 0; c < layout[p].channels; ++c) {
                    if (input && !layout[p].main) {
                        pointers[p].push_back(m_silence.data()); // sidechains: silence
                    } else {
                        pointers[p].push_back(buffers[p].emplace_back(frames).data());
                    }
                }
                ports[p] = {.data32 = pointers[p].data(),
                            .data64 = nullptr,
                            .channel_count = layout[p].channels,
                            .latency = 0,
                            .constant_mask = 0};
            }
        };
        setUp(m_inLayout, inst->inBuffers, inst->inPointers, inst->inPorts, true);
        setUp(m_outLayout, inst->outBuffers, inst->outPointers, inst->outPorts, false);

        if (!inst->plugin->activate(inst->plugin, sampleRate, 1, static_cast<std::uint32_t>(maxEffectFrames))) {
            m_error = "the plugin couldn't be started at this sample rate";
            return;
        }
        inst->active = true;
    }
}

void ClapEffect::process(float *const *channels, int channelCount, int frames) noexcept
{
    if (!m_error.empty() || m_instances.empty() || frames <= 0 || stoppedHere()) {
        return;
    }
    t_audioThread = true;
    frames = std::min(frames, maxEffectFrames);
    const auto n = static_cast<std::size_t>(frames);
    const std::span<float *const> planes(channels, static_cast<std::size_t>(channelCount));
    const bool perChannel = m_instances.size() > 1;
    for (std::size_t i = 0; i < m_params.size(); ++i) {
        m_block[i] = m_targets[i].load(std::memory_order_relaxed);
    }

    for (std::size_t k = 0; k < m_instances.size(); ++k) {
        Instance &inst = *m_instances[k];
        if (!inst.active) {
            continue;
        }
        if (!inst.processing) {
            if (!inst.plugin->start_processing(inst.plugin)) {
                continue;
            }
            inst.processing = true;
        }
        queueParams(inst);

        // The strip into the main input, and back from the main output.
        std::size_t mainIn = 0;
        std::size_t mainOut = 0;
        for (std::size_t p = 0; p < m_inLayout.size(); ++p) {
            mainIn = m_inLayout[p].main ? p : mainIn;
        }
        for (std::size_t p = 0; p < m_outLayout.size(); ++p) {
            mainOut = m_outLayout[p].main ? p : mainOut;
        }
        const std::span<float *const> into(inst.inPointers[mainIn]);
        const std::span<float *const> from(inst.outPointers[mainOut]);
        if (perChannel) {
            spread(std::span<const float *const>(&planes[k], 1), into, n);
        } else {
            spread(std::span<const float *const>(planes.data(), planes.size()), into, n);
        }

        const clap_process_t process{
            .steady_time = m_steadyTime,
            .frames_count = static_cast<std::uint32_t>(frames),
            .transport = nullptr,
            .audio_inputs = inst.inPorts.data(),
            .audio_outputs = inst.outPorts.data(),
            .audio_inputs_count = static_cast<std::uint32_t>(inst.inPorts.size()),
            .audio_outputs_count = static_cast<std::uint32_t>(inst.outPorts.size()),
            .in_events = &inst.in,
            .out_events = &inst.out,
        };
        if (inst.plugin->process(inst.plugin, &process) == CLAP_PROCESS_ERROR) {
            continue; // leaves the strip as it was
        }
        const std::span<const float *const> outputs(from.data(), from.size());
        if (perChannel) {
            spread(outputs, std::span<float *const>(&planes[k], 1), n);
        } else {
            spread(outputs, planes, n);
        }
    }
    m_steadyTime += frames;
}

void ClapEffect::queueParams(Instance &inst) noexcept
{
    // Parameters that changed since the last block.
    inst.eventCount = 0;
    for (std::size_t i = 0; i < m_params.size(); ++i) {
        const double value = m_block[i];
        if (inst.sent[i] == value) {
            continue;
        }
        inst.sent[i] = value;
        inst.events[inst.eventCount++] = {
            .header = {.size = sizeof(clap_event_param_value_t),
                       .time = 0,
                       .space_id = CLAP_CORE_EVENT_SPACE_ID,
                       .type = CLAP_EVENT_PARAM_VALUE,
                       .flags = 0},
            .param_id = m_params[i].id,
            .cookie = m_params[i].cookie,
            .note_id = -1,
            .port_index = -1,
            .channel = -1,
            .key = -1,
            .value = value,
        };
    }
}

void ClapEffect::bypassed(int /*frames*/) noexcept
{
    // Not processing, so its parameter changes (from the engine, or its own
    // editor) go through flush, which is allowed here: the audio thread,
    // while it's active, and not during process.
    if (!m_error.empty() || m_instances.empty() || stoppedHere()) {
        return;
    }
    t_audioThread = true;
    for (std::size_t i = 0; i < m_params.size(); ++i) {
        m_block[i] = m_targets[i].load(std::memory_order_relaxed);
    }
    for (auto &instance : m_instances) {
        Instance &inst = *instance;
        if (!inst.active || inst.params == nullptr || inst.params->flush == nullptr) {
            continue;
        }
        queueParams(inst);
        inst.params->flush(inst.plugin, &inst.in, &inst.out);
    }
}

void ClapEffect::resume() noexcept
{
    // Back from bypass: what it held from before (a reverb's tail) goes.
    for (auto &instance : m_instances) {
        if (instance->active && instance->processing) {
            instance->plugin->reset(instance->plugin);
        }
    }
}

bool ClapEffect::stoppedHere() noexcept
{
    if (!m_stopRequested.load(std::memory_order_acquire)) {
        return false;
    }
    // Asked to stop (the host is closing): stop processing here, on the
    // audio thread, as CLAP wants, once.
    if (!m_stopped.load(std::memory_order_relaxed)) {
        for (auto &instance : m_instances) {
            if (instance->processing) {
                instance->plugin->stop_processing(instance->plugin);
                instance->processing = false;
            }
        }
        m_stopped.store(true, std::memory_order_release);
    }
    return true;
}

void ClapEffect::mainThreadTick()
{
    // Callbacks the plugin asked for (from any thread), here on the main one.
    for (auto &instance : m_instances) {
        if (instance->callback.exchange(false)) {
            instance->plugin->on_main_thread(instance->plugin);
        }
    }
    if (ClapWindow *window = m_window.load()) {
        window->tick();
    }
}

void ClapEffect::setParam(const std::string &name, double value)
{
    clap_id id = 0;
    if (std::from_chars(name.data(), name.data() + name.size(), id).ec != std::errc{} || // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        !std::isfinite(value)) {
        return;
    }
    const auto at = m_paramIndex.find(id);
    if (at == m_paramIndex.end()) {
        return;
    }
    const auto &p = m_params[at->second];
    m_targets[at->second].store(std::clamp(value, p.minimum, p.maximum));
}

void ClapEffect::takeParamChanges(const std::function<void(const std::string &, double)> &fn)
{
    for (std::size_t i = 0; i < m_params.size(); ++i) {
        if (m_changed[i].exchange(false)) {
            fn(std::to_string(m_params[i].id), m_targets[i].load());
        }
    }
}

std::string ClapEffect::saveState() const
{
    if (m_instances.empty()) {
        return {};
    }
    const clap_plugin_t *plugin = m_instances.front()->plugin;
    const auto *state =
        static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (state == nullptr) {
        return {};
    }
    QByteArray data;
    const clap_ostream_t stream{
        .ctx = &data,
        .write = [](const clap_ostream_t *s, const void *buffer, std::uint64_t size) -> std::int64_t {
            // A state past this (a plugin gone wrong), or one there's no
            // memory for, is an error the plugin hears about, not a crash.
            constexpr std::uint64_t maxState = std::uint64_t{256} << 20;
            auto *bytes = static_cast<QByteArray *>(s->ctx);
            if (size > maxState - static_cast<std::uint64_t>(bytes->size())) {
                return -1;
            }
            try {
                bytes->append(static_cast<const char *>(buffer), static_cast<qsizetype>(size));
            } catch (const std::bad_alloc &) {
                return -1;
            }
            return static_cast<std::int64_t>(size);
        },
    };
    if (!state->save(plugin, &stream) || data.isEmpty()) {
        return {};
    }
    return data.toBase64().toStdString();
}

std::unique_ptr<PluginWindow>
ClapEffect::openWindow(const std::string &title, std::function<void(const std::string &, double)> onParam)
{
    auto window = std::make_unique<ClapWindow>(*this, title);
    window->onParam = std::move(onParam); // its changes come as parameter events, though
    return window;
}

ClapEffect &ClapEffect::of(const clap_host_t *host)
{
    return *static_cast<Instance *>(host->host_data)->effect;
}

void ClapEffect::requestRestart(const clap_host_t * /*host*/)
{
    // It would need the audio stopped, which isn't ours to do; it carries on
    // as it is. (Not reported from the audio thread, where nothing may block.)
    if (!t_audioThread) {
        std::println(stderr, "patchprime-pluginhost: the plugin asked to be restarted (ignored)");
    }
}

void ClapEffect::requestProcess(const clap_host_t * /*host*/) {} // processing, or flushed when bypassed

void ClapEffect::requestCallback(const clap_host_t *host)
{
    // Any thread, the audio one included: only a flag, picked up by the main
    // thread's tick (each instance its own).
    static_cast<Instance *>(host->host_data)->callback.store(true);
}

const void *ClapEffect::hostExtension(const clap_host_t * /*host*/, const char *id)
{
    static const clap_host_log_t log{
        .log = [](const clap_host_t *, clap_log_severity severity, const char *message) {
            if (t_audioThread) {
                return; // nothing that may block, there
            }
            std::println(stderr, "patchprime-pluginhost: plugin log ({}): {}", static_cast<int>(severity),
                         message != nullptr ? message : "");
        },
    };
    static const clap_host_thread_check_t threadCheck{
        .is_main_thread = [](const clap_host_t *h) {
            return std::this_thread::get_id() == of(h).m_mainThread;
        },
        .is_audio_thread = [](const clap_host_t *) { return t_audioThread; },
    };
    static const clap_host_params_t params{
        .rescan = [](const clap_host_t *, clap_param_rescan_flags) {},
        .clear = [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
        .request_flush = [](const clap_host_t *) {}, // it's always processing
    };
    static const clap_host_audio_ports_t audioPorts{
        .is_rescan_flag_supported = [](const clap_host_t *, std::uint32_t) { return false; },
        .rescan = [](const clap_host_t *, std::uint32_t) {},
    };
    static const clap_host_latency_t latency{.changed = [](const clap_host_t *) {}};
    static const clap_host_state_t state{
        .mark_dirty = [](const clap_host_t *h) { of(h).m_stateDirty.store(true); },
    };
    static const clap_host_gui_t gui{
        .resize_hints_changed = [](const clap_host_t *) {},
        .request_resize = [](const clap_host_t *h, std::uint32_t width, std::uint32_t height) {
            // Any thread: noted, and done on the main thread's next tick
            // (the window only; the editor has resized itself).
            ClapEffect &self = of(h);
            if (self.m_window.load() == nullptr || width == 0 || height == 0) {
                return false;
            }
            self.m_resizeTo.store((std::uint64_t{width} << 32) | height);
            return true;
        },
        .request_show = [](const clap_host_t *h) {
            ClapEffect &self = of(h);
            if (self.m_window.load() == nullptr) {
                return false;
            }
            self.m_showRequested.store(true);
            return true;
        },
        .request_hide = [](const clap_host_t *h) {
            ClapEffect &self = of(h);
            if (self.m_window.load() == nullptr) {
                return false;
            }
            self.m_guiClosed.store(true); // its window closes
            return true;
        },
        .closed = [](const clap_host_t *h, bool wasDestroyed) {
            ClapEffect &self = of(h);
            if (wasDestroyed) {
                self.m_guiDestroyed.store(true);
            }
            self.m_guiClosed.store(true);
        },
    };
    static const clap_host_timer_support_t timers{
        .register_timer = [](const clap_host_t *h, std::uint32_t periodMs, clap_id *timerId) {
            auto *instance = static_cast<Instance *>(h->host_data);
            ClapEffect &self = *instance->effect;
            const auto *ext = static_cast<const clap_plugin_timer_support_t *>(
                instance->plugin->get_extension(instance->plugin, CLAP_EXT_TIMER_SUPPORT));
            if (ext == nullptr || self.m_context == nullptr) {
                return false;
            }
            const clap_id idNow = self.m_nextTimer++;
            auto *timer = new QTimer(self.m_context);
            QObject::connect(timer, &QTimer::timeout, self.m_context, [instance, ext, idNow] {
                ext->on_timer(instance->plugin, idNow);
            });
            timer->start(static_cast<int>(std::max<std::uint32_t>(periodMs, 1)));
            self.m_timers[idNow] = timer;
            *timerId = idNow;
            return true;
        },
        .unregister_timer = [](const clap_host_t *h, clap_id timerId) {
            ClapEffect &self = of(h);
            const auto it = self.m_timers.find(timerId);
            if (it == self.m_timers.end()) {
                return false;
            }
            it->second->stop();
            it->second->deleteLater();
            self.m_timers.erase(it);
            return true;
        },
    };
    static const clap_host_posix_fd_support_t fds{
        .register_fd = [](const clap_host_t *h, int fd, clap_posix_fd_flags_t flags) {
            auto *instance = static_cast<Instance *>(h->host_data);
            ClapEffect &self = *instance->effect;
            const auto *ext = static_cast<const clap_plugin_posix_fd_support_t *>(
                instance->plugin->get_extension(instance->plugin, CLAP_EXT_POSIX_FD_SUPPORT));
            if (ext == nullptr || self.m_context == nullptr || self.m_fds.contains(fd)) {
                return false;
            }
            // A notifier for reading and one for writing, each on when asked.
            FdWatch watch;
            const std::pair<QSocketNotifier::Type, clap_posix_fd_flags_t> kinds[] = { // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
                {QSocketNotifier::Read, CLAP_POSIX_FD_READ}, {QSocketNotifier::Write, CLAP_POSIX_FD_WRITE}};
            for (const auto &[type, flag] : kinds) {
                auto *notifier = new QSocketNotifier(fd, type, self.m_context);
                notifier->setEnabled((flags & flag) != 0);
                QObject::connect(notifier, &QSocketNotifier::activated, self.m_context,
                                 [instance, ext, fd, flag] { ext->on_fd(instance->plugin, fd, flag); });
                (type == QSocketNotifier::Read ? watch.read : watch.write) = notifier;
            }
            self.m_fds[fd] = watch;
            return true;
        },
        .modify_fd = [](const clap_host_t *h, int fd, clap_posix_fd_flags_t flags) {
            const auto it = of(h).m_fds.find(fd);
            if (it == of(h).m_fds.end()) {
                return false;
            }
            it->second.read->setEnabled((flags & CLAP_POSIX_FD_READ) != 0);
            it->second.write->setEnabled((flags & CLAP_POSIX_FD_WRITE) != 0);
            return true;
        },
        .unregister_fd = [](const clap_host_t *h, int fd) {
            ClapEffect &self = of(h);
            const auto it = self.m_fds.find(fd);
            if (it == self.m_fds.end()) {
                return false;
            }
            // Off now (so another may be made for this fd at once), gone soon
            // (it may be what called the plugin).
            for (QSocketNotifier *notifier : {it->second.read, it->second.write}) {
                notifier->setEnabled(false);
                notifier->deleteLater();
            }
            self.m_fds.erase(it);
            return true;
        },
    };

    const std::string_view name(id != nullptr ? id : "");
    if (name == CLAP_EXT_LOG) {
        return &log;
    }
    if (name == CLAP_EXT_THREAD_CHECK) {
        return &threadCheck;
    }
    if (name == CLAP_EXT_PARAMS) {
        return &params;
    }
    if (name == CLAP_EXT_AUDIO_PORTS) {
        return &audioPorts;
    }
    if (name == CLAP_EXT_LATENCY) {
        return &latency;
    }
    if (name == CLAP_EXT_STATE) {
        return &state;
    }
    if (name == CLAP_EXT_GUI) {
        return &gui;
    }
    if (name == CLAP_EXT_TIMER_SUPPORT) {
        return &timers;
    }
    if (name == CLAP_EXT_POSIX_FD_SUPPORT) {
        return &fds;
    }
    return nullptr;
}

// --- ClapWindow ---------------------------------------------------------------

ClapWindow::ClapWindow(ClapEffect &effect, const std::string &title) : m_effect(effect)
{
    if (effect.m_instances.empty() || !effect.error().empty()) {
        m_error = "the plugin isn't running";
        return;
    }
    m_plugin = effect.m_instances.front()->plugin;
    m_gui = static_cast<const clap_plugin_gui_t *>(m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
    if (m_gui == nullptr || !m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_X11, false)) {
        m_error = "this plugin has no window Patch Prime can show";
        return;
    }
    if (!m_window.create(title, 640, 480)) {
        m_error = "there's no display to show it on";
        return;
    }
    effect.m_guiClosed.store(false);
    effect.m_guiDestroyed.store(false);
    effect.m_resizeTo.store(0);
    if (!m_gui->create(m_plugin, CLAP_WINDOW_API_X11, false)) {
        m_error = "its window couldn't be started";
        m_window.destroy();
        return;
    }
    m_created = true;
    effect.m_window.store(this);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (m_gui->get_size(m_plugin, &width, &height) && width > 0 && height > 0) {
        m_window.resize(static_cast<int>(width), static_cast<int>(height));
    }
    clap_window_t window{.api = CLAP_WINDOW_API_X11, .x11 = m_window.id()};
    if (!m_gui->set_parent(m_plugin, &window)) {
        m_error = "its window couldn't be embedded";
        return;
    }
    m_window.setResizable(m_gui->can_resize(m_plugin));
    if (!m_gui->show(m_plugin)) {
        m_error = "its window couldn't be shown";
        return;
    }
    m_shown = true;
    m_window.show();
}

ClapWindow::~ClapWindow()
{
    m_effect.m_window.store(nullptr);
    if (m_created) {
        // A GUI the plugin has destroyed itself is only acknowledged.
        if (m_shown && !m_effect.m_guiDestroyed.load()) {
            m_gui->hide(m_plugin);
        }
        m_gui->destroy(m_plugin);
    }
    m_window.destroy();
}

void ClapWindow::tick()
{
    // What the plugin asked for from any thread, done here on the main one.
    if (const auto size = m_effect.m_resizeTo.exchange(0); size != 0) {
        m_window.resize(static_cast<int>(size >> 32U), static_cast<int>(size & 0xFFFFFFFFU));
    }
    if (m_effect.m_showRequested.exchange(false)) {
        m_window.raise();
    }
}

bool ClapWindow::idle()
{
    if (!m_created || !m_shown || m_effect.m_guiClosed.load() || !m_window.isOpen()) {
        return false;
    }
    const bool open = m_window.pump([this](int width, int height) {
        // The user resized the window: the editor follows if it can,
        // else the window goes back to the editor's size.
        auto w = static_cast<std::uint32_t>(width);
        auto h = static_cast<std::uint32_t>(height);
        if (m_gui->can_resize(m_plugin)) {
            m_gui->adjust_size(m_plugin, &w, &h);
            m_gui->set_size(m_plugin, w, h);
        } else {
            m_gui->get_size(m_plugin, &w, &h);
        }
        if (std::cmp_not_equal(w, width) || std::cmp_not_equal(h, height)) {
            m_window.resize(static_cast<int>(w), static_cast<int>(h));
        }
    });
    m_window.flush();
    return open && !m_effect.m_guiClosed.load();
}

} // namespace pp

// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

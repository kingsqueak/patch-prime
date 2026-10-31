// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "lv2.h"

#include "lv2ui.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/parameters/parameters.h>
#include <lv2/port-props/port-props.h>
#include <lv2/state/state.h>
#include <lv2/units/units.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <span>

namespace pp {

namespace {

// lv2:isSideChain: not in every lv2 release's headers, though plugins use it.
constexpr const char *sideChainUri = LV2_CORE_PREFIX "isSideChain";

// The atom buffer each atom port gets (events to and from the plugin).
constexpr std::size_t atomCapacity = 8192;

// Features we give every plugin, and those that only describe the plugin
// (we never process in place, and run on a realtime thread).
const std::set<std::string> &supportedFeatures()
{
    static const std::set<std::string> features{
        LV2_URID__map,          LV2_URID__unmap,        LV2_OPTIONS__options,
        LV2_BUF_SIZE__boundedBlockLength,               LV2_LOG__log,
        LV2_CORE__inPlaceBroken, LV2_CORE__isLive,      LV2_CORE__hardRTCapable,
    };
    return features;
}

std::string text(const LilvNode *node)
{
    return node != nullptr ? std::string(lilv_node_as_string(node)) : std::string();
}

// Takes a node lilv handed over and frees it.
std::string take(LilvNode *node)
{
    auto s = text(node);
    lilv_node_free(node);
    return s;
}

float number(const LilvNode *node, float fallback)
{
    if (node == nullptr || !(lilv_node_is_float(node) || lilv_node_is_int(node))) {
        return fallback;
    }
    const float v = lilv_node_as_float(node);
    return std::isfinite(v) ? v : fallback;
}

// A unit's symbol: the one its description gives, or a known one.
std::string unitSymbol(LilvWorld *world, const LilvNode *unit, const LilvNode *symbolPredicate)
{
    if (unit == nullptr) {
        return {};
    }
    if (LilvNode *symbol = lilv_world_get(world, unit, symbolPredicate, nullptr)) {
        return take(symbol);
    }
    static const std::map<std::string, std::string> known{
        {"db", "dB"},   {"hz", "Hz"},  {"khz", "kHz"}, {"mhz", "MHz"},  {"ms", "ms"},
        {"s", "s"},     {"pc", "%"},   {"bpm", "BPM"}, {"cent", "ct"}, {"degree", "°"},
        {"semitone12TET", "st"},       {"oct", "oct"}, {"m", "m"},      {"cm", "cm"},
    };
    const std::string uri = lilv_node_as_uri(unit);
    const auto hash = uri.rfind('#');
    const auto it = known.find(hash == std::string::npos ? uri : uri.substr(hash + 1));
    return it != known.end() ? it->second : std::string();
}

// A plugin's category from its class: lv2:CompressorPlugin -> "Compressor",
// lv2:SimulatorPlugin -> "Simulator". From the plugin's own rdf:type, since
// lilv only knows the classes when the LV2 core description is installed
// (it often isn't: it comes with the development files).
std::string category(LilvWorld *world, const LilvPlugin *plugin)
{
    LilvNode *typePredicate = lilv_new_uri(world, LILV_NS_RDF "type");
    LilvNodes *types = lilv_world_find_nodes(world, lilv_plugin_get_uri(plugin), typePredicate, nullptr);
    lilv_node_free(typePredicate);
    std::string uri;
    LILV_FOREACH (nodes, i, types) {
        const LilvNode *type = lilv_nodes_get(types, i);
        if (!lilv_node_is_uri(type)) {
            continue;
        }
        const std::string candidate = lilv_node_as_uri(type);
        if (candidate.starts_with(LV2_CORE_PREFIX) && candidate != LV2_CORE__Plugin) {
            uri = candidate;
            break;
        }
    }
    lilv_nodes_free(types);
    std::string label;
    {
        const auto hash = uri.rfind('#');
        const auto name = hash == std::string::npos ? std::string() : uri.substr(hash + 1);
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (i > 0 && std::isupper(static_cast<unsigned char>(name[i])) != 0 &&
                std::islower(static_cast<unsigned char>(name[i - 1])) != 0) {
                label += ' ';
            }
            label += name[i];
        }
    }
    for (const std::string_view suffix : {" Plugin", "Plugin"}) {
        if (label.ends_with(suffix)) {
            label.resize(label.size() - suffix.size());
        }
    }
    if (label == "Para EQ") {
        return "Parametric EQ";
    }
    if (label == "Multi EQ") {
        return "Graphic EQ";
    }
    return label.empty() ? std::string("Other") : label;
}

// log:log, to stderr (which the engine forwards to its journal).
int logVprintf(LV2_Log_Handle /*handle*/, LV2_URID /*type*/, const char *format, va_list args) // NOLINT(cppcoreguidelines-pro-type-vararg)
{
    return std::vfprintf(stderr, format, args); // NOLINT(cppcoreguidelines-pro-type-vararg,clang-analyzer-valist.Uninitialized)
}

int logPrintf(LV2_Log_Handle handle, LV2_URID type, const char *format, ...) // NOLINT(cppcoreguidelines-pro-type-vararg,cert-dcl50-cpp)
{
    va_list args; // NOLINT(cppcoreguidelines-init-variables)
    va_start(args, format); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const int n = logVprintf(handle, type, format, args); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    va_end(args); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return n;
}

} // namespace

// --- Lv2World -----------------------------------------------------------------

Lv2World::Lv2World() : m_world(lilv_world_new())
{
    lilv_world_load_all(m_world);
    m_uris = {
        .audio = lilv_new_uri(m_world, LV2_CORE__AudioPort),
        .control = lilv_new_uri(m_world, LV2_CORE__ControlPort),
        .atom = lilv_new_uri(m_world, LV2_ATOM__AtomPort),
        .cv = lilv_new_uri(m_world, LV2_CORE__CVPort),
        .input = lilv_new_uri(m_world, LV2_CORE__InputPort),
        .output = lilv_new_uri(m_world, LV2_CORE__OutputPort),
        .sideChain = lilv_new_uri(m_world, sideChainUri),
        .toggled = lilv_new_uri(m_world, LV2_CORE__toggled),
        .integer = lilv_new_uri(m_world, LV2_CORE__integer),
        .enumeration = lilv_new_uri(m_world, LV2_CORE__enumeration),
        .logarithmic = lilv_new_uri(m_world, LV2_PORT_PROPS__logarithmic),
        .notOnGui = lilv_new_uri(m_world, LV2_PORT_PROPS__notOnGUI),
        .unit = lilv_new_uri(m_world, LV2_UNITS__unit),
        .unitSymbol = lilv_new_uri(m_world, LV2_UNITS__symbol),
        .author = nullptr,
    };

    m_map = {.handle = this, .map = [](LV2_URID_Map_Handle handle, const char *uri) {
                 return static_cast<Lv2World *>(handle)->map(uri);
             }};
    m_unmap = {.handle = this, .unmap = [](LV2_URID_Unmap_Handle handle, LV2_URID urid) {
                   return static_cast<Lv2World *>(handle)->unmap(urid);
               }};
    m_log = {.handle = this, .printf = logPrintf, .vprintf = logVprintf};
    m_mapFeature = {.URI = LV2_URID__map, .data = &m_map};
    m_unmapFeature = {.URI = LV2_URID__unmap, .data = &m_unmap};
    m_logFeature = {.URI = LV2_LOG__log, .data = &m_log};
    m_boundedFeature = {.URI = LV2_BUF_SIZE__boundedBlockLength, .data = nullptr};
    m_optionsFeature = {.URI = LV2_OPTIONS__options, .data = nullptr};
}

Lv2World::~Lv2World()
{
    for (LilvNode *node : {m_uris.audio, m_uris.control, m_uris.atom, m_uris.cv, m_uris.input,
                           m_uris.output, m_uris.sideChain, m_uris.toggled, m_uris.integer,
                           m_uris.enumeration, m_uris.logarithmic, m_uris.notOnGui, m_uris.unit,
                           m_uris.unitSymbol}) {
        lilv_node_free(node);
    }
    lilv_world_free(m_world);
}

LV2_URID Lv2World::map(const char *uri)
{
    const std::scoped_lock lock(m_mapLock);
    const auto [it, added] = m_ids.try_emplace(uri, static_cast<LV2_URID>(m_names.size() + 1));
    if (added) {
        m_names.emplace_back(uri);
    }
    return it->second;
}

const char *Lv2World::unmap(LV2_URID urid)
{
    const std::scoped_lock lock(m_mapLock);
    return urid == 0 || urid > m_names.size() ? nullptr : m_names[urid - 1].c_str();
}

std::vector<const LV2_Feature *> Lv2World::features(const LV2_Options_Option *options)
{
    m_optionsFeature.data = const_cast<LV2_Options_Option *>(options); // NOLINT(cppcoreguidelines-pro-type-const-cast): LV2's feature data isn't const
    return {&m_mapFeature, &m_unmapFeature, &m_logFeature, &m_boundedFeature, &m_optionsFeature,
            nullptr};
}

const LilvPlugin *Lv2World::find(const std::string &uri) const
{
    LilvNode *node = lilv_new_uri(m_world, uri.c_str());
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(m_world), node);
    lilv_node_free(node);
    return plugin;
}

std::string Lv2World::problem(const LilvPlugin *plugin)
{
    LilvNodes *required = lilv_plugin_get_required_features(plugin);
    std::string missing;
    LILV_FOREACH (nodes, i, required) {
        const std::string uri = lilv_node_as_uri(lilv_nodes_get(required, i));
        if (!supportedFeatures().contains(uri)) {
            missing = uri;
            break;
        }
    }
    lilv_nodes_free(required);
    if (!missing.empty()) {
        return "it needs a feature Patch Prime doesn't provide (" + missing + ")";
    }
    return {};
}

PluginInfo Lv2World::describe(const LilvPlugin *plugin) const
{
    PluginInfo info;
    info.type = pluginType(lilv_node_as_uri(lilv_plugin_get_uri(plugin)));
    info.name = take(lilv_plugin_get_name(plugin));
    info.author = take(lilv_plugin_get_author_name(plugin));
    info.category = category(m_world, plugin);

    const auto ports = lilv_plugin_get_num_ports(plugin);
    for (std::uint32_t i = 0; i < ports; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        const bool input = lilv_port_is_a(plugin, port, m_uris.input);
        if (lilv_port_is_a(plugin, port, m_uris.audio)) {
            if (lilv_port_has_property(plugin, port, m_uris.sideChain)) {
                continue;
            }
            ++(input ? info.audioIn : info.audioOut);
            continue;
        }
        if (!input || !lilv_port_is_a(plugin, port, m_uris.control) ||
            lilv_port_has_property(plugin, port, m_uris.notOnGui)) {
            continue;
        }
        LilvNode *def = nullptr;
        LilvNode *min = nullptr;
        LilvNode *max = nullptr;
        lilv_port_get_range(plugin, port, &def, &min, &max);
        ParamInfo p;
        p.name = text(lilv_port_get_symbol(plugin, port));
        p.label = take(lilv_port_get_name(plugin, port));
        p.minimum = number(min, 0.0F);
        p.maximum = number(max, 1.0F);
        if (p.maximum < p.minimum) {
            std::swap(p.minimum, p.maximum);
        }
        p.initial = std::clamp<double>(number(def, static_cast<float>(p.minimum)), p.minimum,
                                       p.maximum);
        lilv_node_free(def);
        lilv_node_free(min);
        lilv_node_free(max);
        LilvNode *unit = lilv_port_get(plugin, port, m_uris.unit);
        p.unit = unitSymbol(m_world, unit, m_uris.unitSymbol);
        lilv_node_free(unit);

        if (lilv_port_has_property(plugin, port, m_uris.toggled)) {
            p.kind = ParamKind::Toggle;
            p.minimum = 0.0;
            p.maximum = 1.0;
        } else if (lilv_port_has_property(plugin, port, m_uris.enumeration)) {
            // A choice when its values are 0, 1, 2...: our choices index
            // their options.
            std::vector<std::pair<double, std::string>> points;
            LilvScalePoints *scale = lilv_port_get_scale_points(plugin, port);
            LILV_FOREACH (scale_points, s, scale) {
                const LilvScalePoint *point = lilv_scale_points_get(scale, s);
                points.emplace_back(number(lilv_scale_point_get_value(point), 0.0F),
                                    text(lilv_scale_point_get_label(point)));
            }
            lilv_scale_points_free(scale);
            std::ranges::sort(points);
            bool counting = !points.empty();
            for (std::size_t k = 0; k < points.size(); ++k) {
                counting = counting && points[k].first == static_cast<double>(k);
            }
            if (counting) {
                p.kind = ParamKind::Choice;
                p.minimum = 0.0;
                p.maximum = static_cast<double>(points.size() - 1);
                for (auto &[value, label] : points) {
                    p.options.push_back(std::move(label));
                }
            }
        } else if (p.minimum > 0.0 && (lilv_port_has_property(plugin, port, m_uris.logarithmic) ||
                                       p.unit == "Hz" || p.unit == "kHz")) {
            p.kind = ParamKind::Log;
        }
        info.params.push_back(std::move(p));
    }

    info.hasUi = findX11Ui(m_world, plugin).has_value();
    info.problem = problem(plugin);
    if (info.problem.empty() && (info.audioIn == 0 || info.audioOut == 0)) {
        info.problem = "it doesn't process audio (an instrument, or an analyser)";
    }
    return info;
}

std::vector<PluginInfo> Lv2World::scan() const
{
    std::vector<PluginInfo> out;
    const LilvPlugins *plugins = lilv_world_get_all_plugins(m_world);
    LILV_FOREACH (plugins, i, plugins) {
        out.push_back(describe(lilv_plugins_get(plugins, i)));
    }
    return out;
}

// --- Lv2Effect ----------------------------------------------------------------

Lv2Effect::Lv2Effect(Lv2World &world, const LilvPlugin *plugin)
    : m_world(world), m_plugin(plugin),
      m_type(pluginType(lilv_node_as_uri(lilv_plugin_get_uri(plugin))))
{
    const auto &uris = world.uris();
    const auto count = lilv_plugin_get_num_ports(plugin);
    for (std::uint32_t i = 0; i < count; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        const bool input = lilv_port_is_a(plugin, port, uris.input);
        Port p{.index = i,
               .kind = PortKind::Other,
               .symbol = text(lilv_port_get_symbol(plugin, port)),
               .initial = 0.0F,
               .minimum = 0.0F,
               .maximum = 1.0F};
        if (lilv_port_is_a(plugin, port, uris.audio)) {
            p.kind = input ? PortKind::AudioIn : PortKind::AudioOut;
            if (lilv_port_has_property(plugin, port, uris.sideChain)) {
                p.kind = PortKind::SideIn;
            }
            if (p.kind == PortKind::SideIn && !input) {
                p.kind = PortKind::Other; // a sidechain output: nothing reads it
            }
        } else if (lilv_port_is_a(plugin, port, uris.control)) {
            p.kind = input ? PortKind::ControlIn : PortKind::ControlOut;
            LilvNode *def = nullptr;
            LilvNode *min = nullptr;
            LilvNode *max = nullptr;
            lilv_port_get_range(plugin, port, &def, &min, &max);
            p.minimum = number(min, 0.0F);
            p.maximum = number(max, 1.0F);
            if (p.maximum < p.minimum) {
                std::swap(p.minimum, p.maximum);
            }
            p.initial = std::clamp(number(def, p.minimum), p.minimum, p.maximum);
            lilv_node_free(def);
            lilv_node_free(min);
            lilv_node_free(max);
        } else if (lilv_port_is_a(plugin, port, uris.atom)) {
            p.kind = input ? PortKind::AtomIn : PortKind::AtomOut;
        }
        if (p.kind == PortKind::AudioIn) {
            m_audioIn.push_back(i);
        } else if (p.kind == PortKind::AudioOut) {
            m_audioOut.push_back(i);
        }
        m_ports.push_back(std::move(p));
    }
    m_targets = std::make_unique<std::atomic<float>[]>(m_ports.size()); // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    for (const auto &p : m_ports) {
        m_targets[p.index].store(p.initial);
        if (p.kind == PortKind::ControlIn) {
            m_bySymbol[p.symbol] = p.index;
        }
    }
    m_sequence = world.map(LV2_ATOM__Sequence);
    m_chunk = world.map(LV2_ATOM__Chunk);
    int atomIns = 0;
    int atomOuts = 0;
    m_atomSlot.assign(m_ports.size(), -1);
    for (const auto &p : m_ports) {
        if (p.kind == PortKind::AtomIn) {
            m_atomSlot[p.index] = atomIns++;
        } else if (p.kind == PortKind::AtomOut) {
            m_atomSlot[p.index] = atomOuts++;
        }
    }
    m_rtScratch.resize(atomCapacity);
    m_uiScratch.resize(atomCapacity);
    m_eventScratch.resize(atomCapacity + sizeof(LV2_Atom_Event));
}

Lv2Effect::~Lv2Effect()
{
    free();
}

void Lv2Effect::free()
{
    for (auto &inst : m_instances) {
        if (inst.instance != nullptr) {
            lilv_instance_deactivate(inst.instance);
            lilv_instance_free(inst.instance);
        }
    }
    m_instances.clear();
}

void Lv2Effect::prepare(double sampleRate, int channels)
{
    free();
    m_error.clear();
    m_channels = channels;
    m_rate = static_cast<float>(sampleRate);
    if (m_audioIn.empty() || m_audioOut.empty()) {
        m_error = "it has no audio input or output";
        return;
    }
    const LV2_URID intType = m_world.map(LV2_ATOM__Int);
    const LV2_URID floatType = m_world.map(LV2_ATOM__Float);
    m_options = {
        {LV2_OPTIONS_INSTANCE, 0, m_world.map(LV2_BUF_SIZE__maxBlockLength), sizeof(std::int32_t),
         intType, &m_blockMax},
        {LV2_OPTIONS_INSTANCE, 0, m_world.map(LV2_BUF_SIZE__minBlockLength), sizeof(std::int32_t),
         intType, &m_blockMin},
        {LV2_OPTIONS_INSTANCE, 0, m_world.map(LV2_PARAMETERS__sampleRate), sizeof(float), floatType,
         &m_rate},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    const auto features = m_world.features(m_options.data());

    const auto frames = static_cast<std::size_t>(maxEffectFrames);
    m_silence.assign(frames, 0.0F);
    m_scratch.assign(frames, 0.0F);
    // A mono plugin on a stereo strip: one per channel.
    const bool perChannel = m_audioIn.size() == 1 && m_audioOut.size() == 1 && channels > 1;
    const int count = perChannel ? channels : 1;
    for (int n = 0; n < count; ++n) {
        Instance inst;
        inst.instance = lilv_plugin_instantiate(m_plugin, sampleRate, features.data());
        if (inst.instance == nullptr) {
            m_error = "the plugin couldn't be started";
            free();
            return;
        }
        inst.controls.assign(m_ports.size(), 0.0F);
        inst.audioIn.assign(m_audioIn.size(), std::vector<float>(frames));
        inst.audioOut.assign(m_audioOut.size(), std::vector<float>(frames));
        std::size_t audioIn = 0;
        std::size_t audioOut = 0;
        for (const auto &p : m_ports) {
            void *buffer = nullptr;
            switch (p.kind) {
            case PortKind::AudioIn:
                buffer = inst.audioIn[audioIn++].data();
                break;
            case PortKind::AudioOut:
                buffer = inst.audioOut[audioOut++].data();
                break;
            case PortKind::SideIn:
                buffer = m_silence.data();
                break;
            case PortKind::ControlIn:
            case PortKind::ControlOut:
                inst.controls[p.index] = m_targets[p.index].load();
                buffer = &inst.controls[p.index];
                break;
            case PortKind::AtomIn:
                buffer = inst.atomIn.emplace_back(atomCapacity).data();
                break;
            case PortKind::AtomOut:
                buffer = inst.atomOut.emplace_back(atomCapacity).data();
                break;
            case PortKind::Other:
                buffer = m_scratch.data();
                break;
            }
            lilv_instance_connect_port(inst.instance, p.index, buffer);
        }
        // Its own state from before (a file it had loaded, say); its ports
        // come from the parameters.
        if (!m_initialState.empty() && hasState()) {
            if (LilvState *state = lilv_state_new_from_string(m_world.world(), m_world.mapFeature(),
                                                              m_initialState.c_str())) {
                lilv_state_restore(
                    state, inst.instance,
                    [](const char * /*symbol*/, void * /*data*/, const void * /*value*/,
                       std::uint32_t /*size*/, std::uint32_t /*type*/) {},
                    nullptr, 0, features.data());
                lilv_state_free(state);
            }
        }
        lilv_instance_activate(inst.instance);
        m_instances.push_back(std::move(inst));
    }
    // The vectors moved, but their buffers didn't: the connections still hold.
}

void Lv2Effect::process(float *const *channels, int channelCount, int frames) noexcept
{
    if (m_instances.empty() || frames <= 0) {
        return;
    }
    frames = std::min(frames, maxEffectFrames);
    const auto n = static_cast<std::size_t>(frames);
    const std::span<float *const> planes(channels, static_cast<std::size_t>(channelCount));
    const bool perChannel = m_instances.size() > 1;

    for (std::size_t k = 0; k < m_instances.size(); ++k) {
        auto &inst = m_instances[k];
        for (const auto &p : m_ports) {
            if (p.kind == PortKind::ControlIn) {
                inst.controls[p.index] = m_targets[p.index].load(std::memory_order_relaxed);
            }
        }
        for (auto &buffer : inst.atomIn) { // an empty event sequence
            auto *seq = reinterpret_cast<LV2_Atom_Sequence *>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = m_sequence;
            seq->body.unit = 0;
            seq->body.pad = 0;
        }
        for (auto &buffer : inst.atomOut) { // room for the plugin's events
            auto *atom = reinterpret_cast<LV2_Atom *>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            atom->size = static_cast<std::uint32_t>(atomCapacity - sizeof(LV2_Atom));
            atom->type = m_chunk;
        }
        if (k == 0) { // events from the UI, at the start of the block
            m_toPlugin.drain(m_rtScratch, [&](std::uint32_t port, std::span<const std::uint8_t> bytes) {
                if (port >= m_atomSlot.size() || m_atomSlot[port] < 0 || bytes.size() < sizeof(LV2_Atom)) {
                    return;
                }
                auto &buffer = inst.atomIn[static_cast<std::size_t>(m_atomSlot[port])];
                auto *event = reinterpret_cast<LV2_Atom_Event *>(m_eventScratch.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                event->time.frames = 0; // NOLINT(cppcoreguidelines-pro-type-union-access)
                std::memcpy(&event->body, bytes.data(), bytes.size());
                lv2_atom_sequence_append_event(
                    reinterpret_cast<LV2_Atom_Sequence *>(buffer.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                    static_cast<std::uint32_t>(atomCapacity - sizeof(LV2_Atom)), event);
            });
        }

        // Inputs: this instance's channel, or the channels spread over the
        // ports (each port the average of the channels that fall on it).
        const auto ins = inst.audioIn.size();
        for (std::size_t port = 0; port < ins; ++port) {
            float *to = inst.audioIn[port].data();
            if (perChannel) {
                std::memcpy(to, planes[k], n * sizeof(float));
                continue;
            }
            if (ins >= planes.size()) { // as many ports as channels, or more
                std::memcpy(to, planes[port % planes.size()], n * sizeof(float));
                continue;
            }
            std::fill_n(to, n, 0.0F);
            int summed = 0;
            for (std::size_t c = port; c < planes.size(); c += ins) {
                const float *from = planes[c];
                for (std::size_t i = 0; i < n; ++i) {
                    to[i] += from[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }
                ++summed;
            }
            if (summed > 1) {
                const float scale = 1.0F / static_cast<float>(summed);
                for (std::size_t i = 0; i < n; ++i) {
                    to[i] *= scale; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }
            }
        }

        lilv_instance_run(inst.instance, static_cast<std::uint32_t>(frames));

        if (k == 0) {
            // Meters and other outputs, and events for the UI.
            for (const auto &p : m_ports) {
                if (p.kind == PortKind::ControlOut) {
                    m_targets[p.index].store(inst.controls[p.index], std::memory_order_relaxed);
                }
            }
            if (m_forwarding.load(std::memory_order_relaxed)) {
                for (const auto &p : m_ports) {
                    if (p.kind != PortKind::AtomOut) {
                        continue;
                    }
                    auto *seq = reinterpret_cast<LV2_Atom_Sequence *>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                        inst.atomOut[static_cast<std::size_t>(m_atomSlot[p.index])].data());
                    if (seq->atom.type != m_sequence) {
                        continue; // nothing written
                    }
                    LV2_ATOM_SEQUENCE_FOREACH (seq, ev) {
                        // The atom's body follows its header in the sequence.
                        m_toUi.push(p.index, std::span(reinterpret_cast<const std::uint8_t *>(&ev->body), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,clang-analyzer-security.ArrayBound)
                                                       sizeof(LV2_Atom) + ev->body.size));
                    }
                }
            }
        }

        // Outputs back to the channels, the same way round.
        const auto outs = inst.audioOut.size();
        if (perChannel) {
            std::memcpy(planes[k], inst.audioOut[0].data(), n * sizeof(float));
            continue;
        }
        for (std::size_t c = 0; c < planes.size(); ++c) {
            float *to = planes[c];
            if (outs < planes.size()) {
                std::memcpy(to, inst.audioOut[c % outs].data(), n * sizeof(float));
                continue;
            }
            std::fill_n(to, n, 0.0F);
            int summed = 0;
            for (std::size_t port = c; port < outs; port += planes.size()) {
                const float *from = inst.audioOut[port].data();
                for (std::size_t i = 0; i < n; ++i) {
                    to[i] += from[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }
                ++summed;
            }
            if (summed > 1) {
                const float scale = 1.0F / static_cast<float>(summed);
                for (std::size_t i = 0; i < n; ++i) {
                    to[i] *= scale; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }
            }
        }
    }
}

LV2_Handle Lv2Effect::handle() const
{
    return m_instances.empty() ? nullptr : lilv_instance_get_handle(m_instances.front().instance);
}

const LV2_Descriptor *Lv2Effect::descriptor() const
{
    return m_instances.empty() ? nullptr
                               : lilv_instance_get_descriptor(m_instances.front().instance);
}

std::vector<std::uint32_t> Lv2Effect::controlPorts() const
{
    std::vector<std::uint32_t> out;
    for (const auto &p : m_ports) {
        if (p.kind == PortKind::ControlIn || p.kind == PortKind::ControlOut) {
            out.push_back(p.index);
        }
    }
    return out;
}

float Lv2Effect::controlValue(std::uint32_t index) const
{
    return index < m_ports.size() ? m_targets[index].load(std::memory_order_relaxed) : 0.0F;
}

std::string Lv2Effect::setControl(std::uint32_t index, float value)
{
    if (index >= m_ports.size() || m_ports[index].kind != PortKind::ControlIn ||
        !std::isfinite(value)) {
        return {};
    }
    const auto &port = m_ports[index];
    m_targets[index].store(std::clamp(value, port.minimum, port.maximum));
    return port.symbol;
}

bool Lv2Effect::sendAtom(std::uint32_t index, std::span<const std::uint8_t> atom)
{
    if (index >= m_ports.size() || m_ports[index].kind != PortKind::AtomIn ||
        atom.size() < sizeof(LV2_Atom) || atom.size() > atomCapacity / 2) {
        return false;
    }
    return m_toPlugin.push(index, atom);
}

void Lv2Effect::takeEvents(const std::function<void(std::uint32_t, const LV2_Atom &)> &fn)
{
    m_toUi.drain(m_uiScratch, [&](std::uint32_t port, std::span<const std::uint8_t> bytes) {
        if (bytes.size() >= sizeof(LV2_Atom)) {
            fn(port, *reinterpret_cast<const LV2_Atom *>(bytes.data())); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        }
    });
}

bool Lv2Effect::hasState() const
{
    LilvNode *node = lilv_new_uri(m_world.world(), LV2_STATE__interface);
    const bool has = lilv_plugin_has_extension_data(m_plugin, node);
    lilv_node_free(node);
    return has;
}

std::string Lv2Effect::saveState() const
{
    if (m_instances.empty() || !hasState()) {
        return {};
    }
    const auto features = m_world.features(m_options.data());
    // Only the plugin's own state: no port values (those are the parameters).
    LilvState *state = lilv_state_new_from_instance(
        m_plugin, m_instances.front().instance, m_world.mapFeature(), nullptr, nullptr, nullptr,
        nullptr,
        [](const char * /*symbol*/, void * /*data*/, std::uint32_t *size,
           std::uint32_t *type) -> const void * {
            *size = 0;
            *type = 0;
            return nullptr;
        },
        nullptr, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features.data());
    if (state == nullptr) {
        return {};
    }
    char *text = lilv_state_to_string(m_world.world(), m_world.mapFeature(), m_world.unmapFeature(),
                                      state, "urn:patchprime:state", nullptr);
    std::string out = text != nullptr ? std::string(text) : std::string();
    lilv_free(text);
    lilv_state_free(state);
    return out;
}

void Lv2Effect::setParam(const std::string &name, double value)
{
    const auto it = m_bySymbol.find(name);
    if (it == m_bySymbol.end() || !std::isfinite(value)) {
        return;
    }
    const auto &port = m_ports[it->second];
    m_targets[it->second].store(
        std::clamp(static_cast<float>(value), port.minimum, port.maximum));
}

} // namespace pp

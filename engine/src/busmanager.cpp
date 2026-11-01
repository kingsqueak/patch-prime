// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"

#include "devicemodel.h"
#include "pipewiregraph.h"
#include "plugins.h"

#include <QCryptographicHash>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <utility>

Q_LOGGING_CATEGORY(lcBus, "patchprime.bus")

namespace pp {

namespace {

constexpr auto pendingTimeout = std::chrono::seconds(3);
const std::string nodeType = "PipeWire:Interface:Node";

std::uint64_t parseSerial(const std::string &value)
{
    std::uint64_t serial = 0;
    std::from_chars(value.data(), value.data() + value.size(), serial);
    return serial;
}

std::uint32_t parseId(const std::string &value)
{
    std::uint32_t id = 0;
    std::from_chars(value.data(), value.data() + value.size(), id);
    return id;
}

std::string joinPositions(const std::vector<std::string> &positions)
{
    std::string out = "[ ";
    for (std::size_t i = 0; i < positions.size(); ++i) {
        out += (i > 0 ? ", " : "") + positions[i];
    }
    return out + " ]";
}

} // namespace

std::string busNodeName(Direction direction, const std::string &busId)
{
    return std::string(busNamePrefix) + (direction == Direction::Input ? "in." : "out.") + busId;
}

std::string virtualNodeName(const std::string &busId)
{
    return std::string(virtualNamePrefix) + busId;
}

std::string sendNodeName(const std::string &fromBusId, const std::string &toBusId)
{
    return std::string(sendNamePrefix) + fromBusId + "." + toBusId;
}

Properties sendProperties(const std::string &name, const std::vector<std::string> &positions,
                          const std::string &description, const std::string &owner)
{
    return {
        {"factory.name", "support.null-audio-sink"},
        {"node.name", name},
        {"node.description", description},
        {"media.class", std::string(sendMediaClass)},
        {"audio.channels", std::to_string(positions.size())},
        {"audio.position", joinPositions(positions)},
        {"node.autoconnect", "false"},
        {"node.virtual", "true"},
        {"monitor.channel-volumes", "true"},
        {"object.linger", "true"},
        {"priority.session", "0"},
        {"priority.driver", "0"},
        {"patchprime.owner", owner},
    };
}

float dbToLinear(double db, double minimumDb)
{
    if (db <= minimumDb) {
        return 0.0F;
    }
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

std::vector<std::string> busPositions(const Endpoint &endpoint)
{
    const int channels = endpoint.channels > 0 ? endpoint.channels : 2;
    if (std::cmp_equal(endpoint.positions.size(), channels)) {
        return endpoint.positions;
    }
    if (channels == 1) {
        return {"MONO"};
    }
    if (channels == 2) {
        return {"FL", "FR"};
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(channels));
    for (int i = 0; i < channels; ++i) {
        out.push_back("AUX" + std::to_string(i));
    }
    return out;
}

std::vector<PortRef> selectPair(const std::vector<PortRef> &ports, int pair)
{
    const auto first = static_cast<std::size_t>(std::max(pair - 1, 0)) * 2;
    if (pair <= 0 || first + 2 > ports.size()) {
        return ports;
    }
    const auto begin = ports.begin() + static_cast<std::ptrdiff_t>(first);
    return {begin, begin + 2};
}

CreationLimiter::Decision CreationLimiter::attempt(const std::string &name, Clock::time_point now)
{
    auto &a = m_attempts[name];
    if (a.count == 0 || now - a.first >= window) {
        a = Attempts{.first = now, .count = 0};
    }
    ++a.count;
    if (a.count <= maxPerWindow) {
        return Decision::Create;
    }
    return a.count == maxPerWindow + 1 ? Decision::RefuseFirst : Decision::Refuse;
}

std::string busOwnerFor(const QString &storePath)
{
    return QCryptographicHash::hash(storePath.toUtf8(), QCryptographicHash::Sha1)
        .toHex()
        .left(8)
        .toStdString();
}

std::string nameSlug(const std::string &label)
{
    std::string slug;
    for (const char ch : label) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 && c < 0x80) {
            slug += static_cast<char>(std::tolower(c));
        } else if (!slug.empty() && slug.back() != '-') {
            slug += '-';
        }
        if (slug.size() >= 32) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? "device" : slug;
}

std::string readableBusName(const DeviceStore &store, const std::string &key)
{
    const auto *s = store.find(key);
    if (s == nullptr) {
        return {};
    }
    auto slug = nameSlug(s->label.empty() ? s->description : s->label);
    // Names Patch Prime uses for other things.
    if (slug == "bus" || slug == "send" || slug == "virtual" || slug == "meter" || slug == "in" ||
        slug == "out") {
        slug += "-1";
    }
    const std::string base =
        s->kind != EndpointKind::Hardware
            ? std::string("patchprime.") + slug
            : std::string(s->direction == Direction::Input ? "patchprime.in." : "patchprime.out.") +
                  slug;
    std::set<std::string> taken;
    for (const auto &[other, settings] : store.entries()) {
        if (other != key && !settings.busName.empty()) {
            taken.insert(settings.busName);
        }
    }
    auto name = base;
    for (int n = 2; taken.contains(name); ++n) {
        name = base + "-" + std::to_string(n);
    }
    return name;
}

std::map<std::string, std::string> migrateBusNames(DeviceStore &store)
{
    std::map<std::string, std::string> renamed;
    for (const auto &[key, settings] : store.entries()) {
        if (!settings.busName.empty() || settings.busId.empty()) {
            continue;
        }
        const auto old = settings.kind != EndpointKind::Hardware
                             ? virtualNodeName(settings.busId)
                             : busNodeName(settings.direction, settings.busId);
        const auto name = readableBusName(store, key);
        auto &s = store.upsert(key);
        s.busName = name;
        if (s.kind != EndpointKind::Hardware) {
            s.nodeName = name; // a virtual device is its own bus
        }
        renamed[old] = name;
    }
    return renamed;
}

std::string busNameFor(const Endpoint &endpoint, const DeviceSettings &settings)
{
    if (!settings.busName.empty()) {
        return settings.busName;
    }
    return settings.kind == EndpointKind::Hardware ? busNodeName(endpoint.direction, settings.busId)
                                                   : virtualNodeName(settings.busId);
}

std::string busMediaClass(const Endpoint &endpoint, const DeviceSettings &settings)
{
    switch (settings.kind) {
    case EndpointKind::VirtualSink:
    case EndpointKind::VirtualInput: // apps play into both; only the routing differs
        return "Audio/Sink";
    case EndpointKind::VirtualSource:
        return "Audio/Source/Virtual";
    case EndpointKind::Hardware:
        break;
    }
    // An input bus is a source, so apps can record the processed input from it
    // and it doesn't show up in the list of outputs.
    return endpoint.direction == Direction::Input ? "Audio/Source/Virtual" : "Audio/Sink";
}

std::string busDescription(const Endpoint &endpoint, const DeviceSettings &settings)
{
    const auto &name = settings.label.empty() ? endpoint.description : settings.label;
    if (settings.kind != EndpointKind::Hardware) {
        return name; // the user's own device: just its name
    }
    return (endpoint.direction == Direction::Input ? "Patch Prime In: " : "Patch Prime Out: ") +
           name;
}

Properties busProperties(const Endpoint &endpoint, const DeviceSettings &settings,
                         const std::string &owner)
{
    const auto positions = busPositions(endpoint);
    return {
        {"factory.name", "support.null-audio-sink"},
        {"node.name", busNameFor(endpoint, settings)},
        {"node.description", busDescription(endpoint, settings)},
        {"media.class", busMediaClass(endpoint, settings)},
        {"audio.channels", std::to_string(positions.size())},
        {"audio.position", joinPositions(positions)},
        {"node.autoconnect", "false"},
        {"node.virtual", "true"},
        // The fader applies to what leaves the bus (the monitor/capture ports).
        {"monitor.channel-volumes", "true"},
        {"object.linger", "true"},
        // Low priority so WirePlumber never picks a bus as the default device.
        {"priority.session", "0"},
        {"priority.driver", std::to_string(busDriverPriority)},
        {"patchprime.key", endpoint.key},
        {"patchprime.owner", owner},
    };
}

std::vector<std::pair<PortRef, PortRef>> pairPorts(const std::vector<PortRef> &outputs,
                                                   const std::vector<PortRef> &inputs)
{
    std::vector<std::pair<PortRef, PortRef>> out;
    if (outputs.empty() || inputs.empty()) {
        return out;
    }
    if (outputs.size() == 1) {
        for (const auto &in : inputs) {
            out.emplace_back(outputs.front(), in);
        }
        return out;
    }
    if (inputs.size() == 1) {
        for (const auto &o : outputs) {
            out.emplace_back(o, inputs.front());
        }
        return out;
    }

    // By channel name, when both sides name the same channels.
    auto names = [](const std::vector<PortRef> &ports) {
        std::set<std::string> set;
        for (const auto &p : ports) {
            set.insert(p.channel);
        }
        return set;
    };
    const auto outNames = names(outputs);
    if (outNames.size() == outputs.size() && !outNames.contains("") && outNames == names(inputs) &&
        inputs.size() == outputs.size()) {
        for (const auto &o : outputs) {
            const auto it = std::ranges::find(inputs, o.channel, &PortRef::channel);
            out.emplace_back(o, *it);
        }
        return out;
    }

    for (std::size_t i = 0; i < std::min(outputs.size(), inputs.size()); ++i) {
        out.emplace_back(outputs[i], inputs[i]);
    }
    return out;
}

BusManager::BusManager(PipeWireGraph *graph, DeviceModel *devices, QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices),
      m_owner(busOwnerFor(devices->store().path())), m_runner(new PluginRunner(this))
{
    connect(m_runner, &PluginRunner::changed, this, [this] {
        scheduleReconcile();
        Q_EMIT pluginsChanged();
    });
    // What a plugin's window changes is kept like any change of its settings.
    connect(m_runner, &PluginRunner::paramChanged, this,
            [this](const std::string &node, const std::string &type, const std::string &name,
                   double value) {
                if (const int row = pluginRow(node, type); row >= 0) {
                    m_devices->setEffectParam(row, static_cast<int>(m_plugins.at(node).second),
                                              QString::fromStdString(name), value);
                }
            });
    connect(m_runner, &PluginRunner::stateChanged, this,
            [this](const std::string &node, const std::string &type, const std::string &state) {
                if (const int row = pluginRow(node, type); row >= 0) {
                    m_devices->setEffectState(row, static_cast<int>(m_plugins.at(node).second), state);
                }
            });
    // A scan that finished (or a cache that loaded) can let plugins start
    // that couldn't be found before.
    if (const auto *catalog = devices->pluginCatalog()) {
        connect(catalog, &PluginCatalog::changed, this, &BusManager::scheduleReconcile);
    }
    // Graph events come in bursts (a device brings a node and its ports);
    // reconcile once after the burst.
    m_reconcileTimer.setSingleShot(true);
    m_reconcileTimer.setInterval(20);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &BusManager::reconcile);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(pendingTimeout + std::chrono::milliseconds(200));
    connect(&m_retryTimer, &QTimer::timeout, this, &BusManager::scheduleReconcile);

    connect(graph, &PipeWireGraph::objectAdded, this, &BusManager::scheduleReconcile);
    connect(graph, &PipeWireGraph::objectChanged, this, &BusManager::scheduleReconcile);
    connect(graph, &PipeWireGraph::objectRemoved, this, [this](const GraphObject &object) {
        m_applied.erase(object.id);
        scheduleReconcile();
    });
    connect(graph, &PipeWireGraph::connectedChanged, this, &BusManager::onConnectedChanged);
    connect(devices, &DeviceModel::settingsChanged, this, &BusManager::scheduleReconcile);
    connect(devices, &DeviceModel::cueChanged, this, &BusManager::scheduleReconcile);
    scheduleReconcile();
}

namespace {

std::string_view withoutPrefix(const std::string &busName)
{
    constexpr std::string_view prefix = "patchprime.";
    return std::string_view(busName).starts_with(prefix)
               ? std::string_view(busName).substr(prefix.size())
               : std::string_view(busName);
}

} // namespace

std::string effectsNodeName(const std::string &busName, int run)
{
    auto name = "patchprime.fx." + std::string(withoutPrefix(busName));
    return run <= 1 ? name : name + "." + std::to_string(run);
}

std::string pluginNodeName(const std::string &busName, int plugin)
{
    return "patchprime.plug." + std::string(withoutPrefix(busName)) + "." + std::to_string(plugin);
}

bool reaches(const std::map<std::string, std::vector<std::string>> &sends,
             const std::string &from, const std::string &to)
{
    std::set<std::string> seen;
    std::vector<std::string> todo{from};
    while (!todo.empty()) {
        const auto at = std::move(todo.back());
        todo.pop_back();
        if (at == to) {
            return true;
        }
        if (!seen.insert(at).second) {
            continue;
        }
        if (const auto next = sends.find(at); next != sends.end()) {
            todo.insert(todo.end(), next->second.begin(), next->second.end());
        }
    }
    return false;
}

std::unique_ptr<EffectChain> buildChain(const std::vector<EffectSettings> &effects,
                                        double sampleRate, int channels)
{
    std::vector<std::unique_ptr<Effect>> made;
    for (const auto &e : effects) {
        auto effect = makeEffect(e.type);
        if (!effect) {
            continue;
        }
        for (const auto &[name, value] : e.params) {
            effect->setParam(name, value);
        }
        effect->enabled = e.enabled;
        made.push_back(std::move(effect));
    }
    return std::make_unique<EffectChain>(std::move(made), sampleRate, channels);
}

bool updateChain(EffectChain &chain, const std::vector<EffectSettings> &effects)
{
    std::vector<const EffectSettings *> known;
    for (const auto &e : effects) {
        if (makeEffect(e.type)) {
            known.push_back(&e);
        }
    }
    if (known.size() != chain.size()) {
        return false;
    }
    for (std::size_t i = 0; i < known.size(); ++i) {
        if (chain.at(i).type() != known[i]->type) {
            return false;
        }
    }
    for (std::size_t i = 0; i < known.size(); ++i) {
        Effect &effect = chain.at(i);
        for (const auto &[name, value] : known[i]->params) {
            effect.setParam(name, value);
        }
        effect.enabled = known[i]->enabled;
    }
    return true;
}

void BusManager::setMetering(bool enabled)
{
    if (enabled == m_metering) {
        return;
    }
    m_metering = enabled;
    if (!enabled) {
        for (const auto &[name, meter] : m_meters) {
            m_graph->removeMeter(meter.second);
        }
        m_meters.clear();
    }
    scheduleReconcile();
}

std::map<std::string, std::vector<float>> BusManager::takeLevels()
{
    std::map<std::string, std::vector<float>> out;
    for (const auto &[name, meter] : m_meters) {
        out[meter.first] = meter.second->take();
    }
    return out;
}

std::map<std::string, BusManager::Loudness> BusManager::loudness() const
{
    std::map<std::string, Loudness> out;
    for (const auto &[name, meter] : m_meters) {
        out[meter.first] = {.rms = meter.second->rmsNow(),
                            .lufs = meter.second->loudness.load(std::memory_order_relaxed)};
    }
    return out;
}

std::map<std::pair<std::string, std::size_t>, std::vector<float>> BusManager::takeEffectMeters()
{
    std::map<std::pair<std::string, std::size_t>, std::vector<float>> out;
    std::array<float, 16> buffer{};
    for (const auto &[name, fx] : m_effects) {
        auto *chain = fx.processor->chain(); // only swapped on this thread
        if (chain == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < chain->size() && i < fx.indices.size(); ++i) {
            if (const auto n = chain->at(i).takeMeters(buffer); n > 0) {
                out[{fx.key, fx.indices[i]}] = std::vector<float>(buffer.begin(),
                                                      buffer.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }
    }
    return out;
}

void BusManager::setEffectMetering(std::set<std::string> keys)
{
    m_effectMetering = std::move(keys);
    for (const auto &[name, fx] : m_effects) {
        fx.processor->setMetering(m_effectMetering.contains(fx.key));
    }
    for (const auto &[node, at] : m_plugins) {
        m_runner->setMetering(node, m_effectMetering.contains(at.first));
    }
}

std::map<std::pair<std::string, std::size_t>, EffectLevels> BusManager::takeEffectLevels()
{
    std::map<std::pair<std::string, std::size_t>, EffectLevels> out;
    for (const auto &[name, fx] : m_effects) {
        auto *chain = fx.processor->chain(); // only swapped on this thread
        if (chain == nullptr || !fx.processor->isMetering()) {
            continue;
        }
        const auto levels = chain->takeLevels();
        for (std::size_t i = 0; i < levels.size() && i < fx.indices.size(); ++i) {
            out[{fx.key, fx.indices[i]}] = levels[i];
        }
    }
    for (const auto &[node, at] : m_plugins) {
        if (!m_effectMetering.contains(at.first)) {
            continue;
        }
        if (auto levels = m_runner->takeLevels(node)) {
            out[at] = *levels;
        }
    }
    return out;
}

int BusManager::pluginRow(const std::string &node, const std::string &type) const
{
    const auto at = m_plugins.find(node);
    if (at == m_plugins.end()) {
        return -1;
    }
    const int row = m_devices->rowForKey(at->second.first);
    const auto *settings = row >= 0 ? m_devices->store().find(at->second.first) : nullptr;
    // Still that plugin at that place (not another moved there since).
    if (settings == nullptr || at->second.second >= settings->effects.size() ||
        settings->effects[at->second.second].type != type) {
        return -1;
    }
    return row;
}

void BusManager::setPluginWindow(const std::string &key, std::size_t index, bool open)
{
    for (const auto &[node, at] : m_plugins) {
        if (at == std::pair{key, index}) {
            m_runner->setWindow(node, open);
        }
    }
}

void BusManager::requestPluginStates()
{
    for (const auto &[node, at] : m_plugins) {
        m_runner->requestState(node);
    }
}

std::set<std::pair<std::string, std::size_t>> BusManager::pluginWindows() const
{
    std::set<std::pair<std::string, std::size_t>> out;
    for (const auto &[node, at] : m_plugins) {
        if (m_runner->isWindowOpen(node)) {
            out.insert(at);
        }
    }
    return out;
}

std::map<std::pair<std::string, std::size_t>, std::string> BusManager::pluginWindowErrors() const
{
    std::map<std::pair<std::string, std::size_t>, std::string> out;
    for (const auto &[node, at] : m_plugins) {
        if (auto error = m_runner->windowError(node); !error.empty()) {
            out[at] = std::move(error);
        }
    }
    return out;
}

std::map<std::pair<std::string, std::size_t>, std::string> BusManager::pluginErrors() const
{
    std::map<std::pair<std::string, std::size_t>, std::string> out;
    for (const auto &[node, at] : m_plugins) {
        if (auto error = m_runner->error(node); !error.empty()) {
            out[at] = std::move(error);
        }
    }
    return out;
}

void BusManager::removeAllBusesIn(PipeWireGraph &graph)
{
    for (const auto &[id, node] : graph.nodes()) {
        // Every bus, virtual device and send node Patch Prime made (they carry
        // an owner tag, or have an old id-based name); not meters.
        if (const auto &name = prop(node, "node.name");
            !prop(node, "patchprime.owner").empty() || name.starts_with(busNamePrefix) ||
            name.starts_with(sendNamePrefix)) {
            graph.destroyObject(id);
        }
    }
}

void BusManager::scheduleReconcile()
{
    if (!m_reconcileTimer.isActive()) {
        m_reconcileTimer.start();
    }
}

void BusManager::onConnectedChanged(bool connected)
{
    // Everything tied to the old connection is gone with it.
    m_pendingBuses.clear();
    m_pendingLinks.clear();
    m_applied.clear();
    m_meters.clear();
    m_effects.clear();
    if (connected) {
        scheduleReconcile();
    }
}

std::map<std::string, BusManager::Wanted> BusManager::wantedBuses() const
{
    std::map<std::string, Wanted> out;
    for (const auto &row : m_devices->rows()) {
        const auto *settings = m_devices->store().find(row.endpoint.key);
        if (settings == nullptr || !settings->enabled) {
            continue;
        }
        Wanted w{
            .key = row.endpoint.key,
            .endpoint = row.endpoint,
            .settings = settings,
            .hardwareNodeId = row.nodeId,
        };
        // Some nodes (e.g. ALSA pro-audio) don't publish audio.channels; their
        // ports say how many channels they have.
        if (w.endpoint.channels == 0 && w.hardwareNodeId) {
            const bool input = w.endpoint.direction == Direction::Input;
            w.endpoint.channels =
                static_cast<int>(portsOf(*w.hardwareNodeId, input ? "out" : "in", false).size());
        }
        if (settings->channelPair > 0 && settings->kind == EndpointKind::Hardware &&
            w.endpoint.channels >= settings->channelPair * 2) {
            w.endpoint.channels = 2; // one stereo pair of a multichannel card
            w.endpoint.positions = {"FL", "FR"};
        }
        if (settings->mono && w.endpoint.direction == Direction::Input) {
            w.endpoint.channels = 1;
            w.endpoint.positions = {"MONO"};
        }
        out[busNameFor(w.endpoint, *settings)] = std::move(w);
    }
    return out;
}

std::vector<PortRef> BusManager::portsOf(std::uint32_t nodeId, const std::string &direction,
                                         bool monitor) const
{
    const auto id = std::to_string(nodeId);
    std::vector<std::pair<std::uint32_t, PortRef>> found;
    for (const auto &[portId, port] : m_graph->ports()) {
        if (prop(port, "node.id") != id || prop(port, "port.direction") != direction ||
            (prop(port, "port.monitor") == "true") != monitor) {
            continue;
        }
        found.emplace_back(
            parseId(prop(port, "port.id")),
            PortRef{.nodeId = nodeId, .portId = portId, .channel = prop(port, "audio.channel")});
    }
    std::ranges::sort(found, {}, &std::pair<std::uint32_t, PortRef>::first);
    std::vector<PortRef> out;
    out.reserve(found.size());
    for (auto &[order, ref] : found) {
        out.push_back(std::move(ref));
    }
    return out;
}

void BusManager::reconcile()
{
    if (!m_graph->isConnected()) {
        return;
    }
    const auto now = Clock::now();
    std::erase_if(m_pendingBuses, [now](const auto &p) { return now - p.second > pendingTimeout; });
    std::erase_if(m_pendingLinks, [now](const auto &p) { return now - p.second > pendingTimeout; });

    const auto wanted = wantedBuses();
    const auto sends = wantedSends(wanted);

    std::map<std::string, std::uint32_t> existing;
    for (const auto &[id, node] : m_graph->nodes()) {
        const auto &name = prop(node, "node.name");
        const auto &owner = prop(node, "patchprime.owner");
        // Ours: tagged with our owner (any name), or untagged with an old
        // id-based name from before the owner tag.
        const bool legacy = owner.empty() && (name.starts_with(busNamePrefix) ||
                                              name.starts_with(virtualNamePrefix) ||
                                              name.starts_with(sendNamePrefix));
        if (owner == m_owner || legacy) {
            // Two of ours with one name (a reconnect that raced the graph's
            // listing used to cause this): keep the older one and remove the
            // extra, or the link cleanup would tear down one copy's routing.
            // Older by object.serial, which only grows; ids are reused.
            if (const auto other = existing.find(name); other != existing.end()) {
                const auto serialOf = [this](std::uint32_t nodeId) {
                    return parseSerial(prop(m_graph->nodes().at(nodeId), "object.serial"));
                };
                const bool keepOther = serialOf(other->second) <= serialOf(id);
                const auto extra = keepOther ? id : other->second;
                qCWarning(lcBus, "removing a duplicate %s (node %u)", name.c_str(), extra);
                m_graph->destroyObject(extra);
                other->second = keepOther ? other->second : id;
                continue;
            }
            existing[name] = id;
        }
    }

    LinkMap existingLinks; // (output port, input port) -> link id
    for (const auto &[id, link] : m_graph->links()) {
        const std::pair ports{parseId(prop(link, "link.output.port")),
                              parseId(prop(link, "link.input.port"))};
        existingLinks[ports] = id;
        m_pendingLinks.erase(ports);
    }

    // Buses nobody wants any more, including leftovers from a crash. If one
    // is the system default output, hand the default back to its hardware
    // first; otherwise WirePlumber would pick some other device.
    for (const auto &[name, id] : existing) {
        if (wanted.contains(name) || sends.contains(name)) {
            continue;
        }
        // A bus that is the system default output or input hands the default
        // back to its hardware first; otherwise WirePlumber picks a device.
        for (const auto direction : {Direction::Output, Direction::Input}) {
            const auto &current = direction == Direction::Output ? m_graph->defaultSinkName()
                                                                 : m_graph->defaultSourceName();
            if (name != current) {
                continue;
            }
            if (const auto to = m_renamed.find(name); to != m_renamed.end()) {
                // Renamed, not removed: the default follows the new name.
                m_graph->setConfiguredDefault(direction, to->second);
                continue;
            }
            const auto &key = prop(m_graph->nodes().at(id), "patchprime.key");
            const int row = m_devices->rowForKey(key);
            // A virtual device has no hardware to hand back to; WirePlumber then
            // picks a device by priority, and buses have the lowest.
            if (row >= 0 && m_devices->rows()[static_cast<std::size_t>(row)].nodeId &&
                m_devices->rows()[static_cast<std::size_t>(row)].endpoint.nodeName != name) {
                m_graph->setConfiguredDefault(
                    direction, m_devices->rows()[static_cast<std::size_t>(row)].endpoint.nodeName);
            }
        }
        qCInfo(lcBus, "removing bus %s", name.c_str());
        m_graph->destroyObject(id);
    }

    // Buses that exist and are wanted, by node name -> node id.
    std::map<std::string, std::uint32_t> ready;

    for (const auto &[name, w] : wanted) {
        const auto it = existing.find(name);
        if (it == existing.end()) {
            if (!m_pendingBuses.contains(name) && mayCreate(name, now)) {
                qCInfo(lcBus, "creating bus %s for %s", name.c_str(), w.key.c_str());
                if (m_graph->createNode(busProperties(w.endpoint, *w.settings, m_owner))) {
                    m_pendingBuses[name] = now;
                }
            }
            continue;
        }
        m_limiter.appeared(name);
        m_pendingBuses.erase(name);
        const std::uint32_t busId = it->second;
        const auto &node = m_graph->nodes().at(busId);

        // A bus made with different settings (an older version, a changed
        // channel count, a renamed virtual device, an older driver priority) is
        // replaced. WirePlumber keeps it as the default if it was, since the
        // name stays the same.
        const auto channels = busPositions(w.endpoint).size();
        const bool stale =
            prop(node, "media.class") != busMediaClass(w.endpoint, *w.settings) ||
            prop(node, "priority.driver") != std::to_string(busDriverPriority) ||
            parseId(prop(node, "audio.channels")) != channels ||
            (w.settings->kind != EndpointKind::Hardware &&
             prop(node, "node.description") != busDescription(w.endpoint, *w.settings));
        if (stale) {
            qCInfo(lcBus, "replacing bus %s", name.c_str());
            m_graph->destroyObject(busId);
            continue;
        }
        ready[name] = busId;

        // Fader and mute; the cue output is muted while something is cued
        // (the headphones then hear only the cue).
        const bool cueing = !m_devices->cued().empty() && w.key == m_devices->store().cueOutput();
        applyVolume(busId, channels, w.settings->volumeDb, w.settings->muted || cueing);

        if (m_metering && !m_meters.contains(name)) {
            const bool sink = busMediaClass(w.endpoint, *w.settings) == "Audio/Sink";
            if (auto levels = m_graph->addMeter(name, static_cast<int>(channels), sink)) {
                m_meters[name] = {w.key, std::move(levels)};
            }
        }
    }

    // Send gain nodes, made and checked like buses.
    std::map<std::string, std::uint32_t> readySends;
    for (const auto &[name, s] : sends) {
        const auto it = existing.find(name);
        if (it == existing.end()) {
            if (!m_pendingBuses.contains(name) && mayCreate(name, now) &&
                m_graph->createNode(sendProperties(name, s.positions, s.description, m_owner))) {
                m_pendingBuses[name] = now;
            }
            continue;
        }
        m_limiter.appeared(name);
        m_pendingBuses.erase(name);
        if (parseId(prop(m_graph->nodes().at(it->second), "audio.channels")) !=
            s.positions.size()) {
            m_graph->destroyObject(it->second); // the sending bus changed shape (mono)
            continue;
        }
        readySends[name] = it->second;
        applyVolume(it->second, s.positions.size(), s.levelDb, false);
    }

    // What each strip feeds the next stage from: its effects node, or its
    // bus (also when the effects come before the bus).
    // The player standing in for a hardware input: that input's device is
    // unlinked while the player is up (see linkHardware).
    m_substitute.clear();
    if (!m_playerNode.empty() && tapNode(m_playerNode)) {
        for (const auto &[name, w] : wanted) {
            if (w.key == m_playerTarget && effectsBeforeBus(w)) {
                m_substitute = w.key;
            }
        }
    }
    const auto fx = reconcileEffects(ready, wanted, existingLinks);
    std::map<std::string, std::vector<PortRef>> stripOuts;
    for (const auto &[name, busId] : ready) {
        const auto &w = wanted.at(name);
        const auto f = fx.find(name);
        const FxPorts *ports = f != fx.end() ? &f->second : nullptr;
        stripOuts[name] = ports != nullptr && !effectsBeforeBus(w) ? ports->out
                                                                   : portsOf(busId, "out", true);
        linkHardware(busId, ports, w, existingLinks);
    }

    linkSends(ready, fx, stripOuts, wanted, sends, readySends, existingLinks);
    linkKeys(fx, wanted, stripOuts, existingLinks);
    linkCue(ready, wanted, stripOuts, existingLinks);
    linkTaps(ready, wanted, fx, stripOuts, existingLinks);

    if ((!m_pendingLinks.empty() || !m_pendingBuses.empty()) && !m_retryTimer.isActive()) {
        m_retryTimer.start();
    }

    // Meters whose bus is gone or no longer wanted.
    for (auto it = m_meters.begin(); it != m_meters.end();) {
        if (!ready.contains(it->first)) {
            m_graph->removeMeter(it->second.second);
            it = m_meters.erase(it);
        } else {
            ++it;
        }
    }
}

bool BusManager::mayCreate(const std::string &name, CreationLimiter::Clock::time_point now)
{
    switch (m_limiter.attempt(name, now)) {
    case CreationLimiter::Decision::Create:
        return true;
    case CreationLimiter::Decision::RefuseFirst:
        qCWarning(lcBus,
                  "%s was created %d times in %lld s but never appeared; not trying again "
                  "for now",
                  name.c_str(), CreationLimiter::maxPerWindow,
                  static_cast<long long>(CreationLimiter::window.count()));
        return false;
    case CreationLimiter::Decision::Refuse:
        break;
    }
    return false;
}

void BusManager::applyVolume(std::uint32_t nodeId, std::size_t channels, double db, bool muted)
{
    const std::vector<float> volumes(channels, dbToLinear(db, DeviceModel::minimumVolumeDb()));
    const std::pair state{volumes, muted};
    if (auto applied = m_applied.find(nodeId);
        applied == m_applied.end() || applied->second != state) {
        if (m_graph->setNodeVolume(nodeId, volumes, muted)) {
            m_applied[nodeId] = state;
        }
    }
}

std::map<std::string, BusManager::WantedSend>
BusManager::wantedSends(const std::map<std::string, Wanted> &buses)
{
    std::map<std::string, const std::pair<const std::string, Wanted> *> byKey;
    for (const auto &entry : buses) {
        byKey[entry.second.key] = &entry;
    }
    std::map<std::string, WantedSend> out;
    for (const auto &[name, w] : buses) {
        if (stageOf(w.endpoint.direction, w.settings->kind) == Stage::Output) {
            continue;
        }
        for (const auto &destination : w.settings->sends) {
            const auto target = byKey.find(destination);
            if (target == byKey.end()) {
                continue; // destination disabled or forgotten
            }
            const auto &[toName, to] = *target->second;
            const auto level = w.settings->sendLevels.find(destination);
            out[sendNodeName(w.settings->busId, to.settings->busId)] = WantedSend{
                .from = name,
                .to = toName,
                .positions = busPositions(w.endpoint),
                .levelDb = level != w.settings->sendLevels.end() ? level->second : 0.0,
                .description = "Patch Prime send: " + busDescription(w.endpoint, *w.settings) +
                               " → " + busDescription(to.endpoint, *to.settings),
            };
        }
    }
    return out;
}

bool BusManager::effectsBeforeBus(const Wanted &wanted)
{
    return wanted.settings->kind == EndpointKind::Hardware &&
           wanted.endpoint.direction == Direction::Input;
}

void BusManager::link(const std::vector<PortRef> &outputs, const std::vector<PortRef> &inputs,
                      const LinkMap &existingLinks)
{
    for (const auto &[out, in] : pairPorts(outputs, inputs)) {
        const std::pair ports{out.portId, in.portId};
        if (!existingLinks.contains(ports) && !m_pendingLinks.contains(ports) &&
            m_graph->createLink(out.nodeId, out.portId, in.nodeId, in.portId, true)) {
            m_pendingLinks[ports] = Clock::now();
        }
    }
}

std::optional<bool> BusManager::ensureEffectsNode(const std::string &fxName, const Wanted &wanted,
                                                  std::vector<EffectSettings> effects,
                                                  std::vector<std::size_t> indices,
                                                  const std::vector<std::string> &positions,
                                                  bool keyed, int rate)
{
    const int channels = static_cast<int>(positions.size());
    auto fx = m_effects.find(fxName);
    if (fx != m_effects.end() && (fx->second.positions != positions || fx->second.keyed != keyed)) {
        qCInfo(lcBus, "replacing effects node %s (ports changed)", fxName.c_str());
        m_graph->removeFilter(fxName);
        m_effects.erase(fx);
        fx = m_effects.end();
    }
    bool created = false;
    if (fx == m_effects.end()) {
        created = true;
        auto processor = std::make_shared<Processor>();
        processor->setChain(buildChain(effects, rate, channels));
        processor->setMetering(m_effectMetering.contains(wanted.key));
        const Properties props{
            {"media.class", std::string(effectsMediaClass)},
            {"node.description",
             "Patch Prime effects: " + busDescription(wanted.endpoint, *wanted.settings)},
            {"patchprime.fx.owner", m_owner},
            {"node.group", "patchprime.fx"},
        };
        qCInfo(lcBus, "creating effects node %s", fxName.c_str());
        if (!m_graph->addFilter(fxName, positions, props, processor, keyed)) {
            return std::nullopt;
        }
        fx = m_effects
                 .emplace(fxName, Fx{.key = wanted.key,
                                     .processor = std::move(processor),
                                     .positions = positions,
                                     .keyed = keyed,
                                     .effects = std::move(effects),
                                     .indices = std::move(indices),
                                     .sampleRate = rate})
                 .first;
    } else if (fx->second.effects != effects || fx->second.sampleRate != rate) {
        auto *chain = fx->second.processor->chain();
        if (fx->second.sampleRate != rate || chain == nullptr || !updateChain(*chain, effects)) {
            fx->second.processor->setChain(buildChain(effects, rate, channels));
        }
        fx->second.effects = std::move(effects);
        fx->second.sampleRate = rate;
        fx->second.indices = std::move(indices);
    } else {
        fx->second.indices = std::move(indices);
    }
    fx->second.processor->collect();
    return created;
}

std::map<std::string, BusManager::FxPorts>
BusManager::reconcileEffects(const std::map<std::string, std::uint32_t> &ready,
                             const std::map<std::string, Wanted> &wanted,
                             const LinkMap &existingLinks)
{
    // Our effects and plugin nodes in the graph, by node name (plugin hosts
    // tag theirs like ours).
    std::map<std::string, std::uint32_t> filterNodes;
    for (const auto &[id, node] : m_graph->nodes()) {
        if (prop(node, "media.class") == effectsMediaClass &&
            prop(node, "patchprime.fx.owner") == m_owner) {
            filterNodes[prop(node, "node.name")] = id;
        }
    }

    const int rate = m_devices->store().sampleRate();
    const auto *catalog = m_devices->pluginCatalog();
    std::map<std::string, FxPorts> up;
    std::set<std::string> keep;
    std::map<std::string, std::pair<std::string, std::size_t>> plugins;
    for (const auto &[name, busId] : ready) {
        const auto &w = wanted.at(name);
        const auto segments = splitChain(w.settings->effects);
        if (segments.empty()) {
            continue;
        }
        const auto positions = busPositions(w.endpoint);
        const auto description = busDescription(w.endpoint, *w.settings);

        struct Part
        {
            std::vector<PortRef> in;
            std::vector<PortRef> out;
            std::vector<PortRef> key;
        };
        std::vector<Part> stages; // the nodes that are up, in order
        FxPorts ports;
        int runs = 0;
        int pluginCount = 0;
        for (const auto &segment : segments) {
            std::string nodeName;
            bool keyed = false;
            bool created = false;
            if (segment.plugin) {
                const auto index = segment.indices.front();
                const auto &e = w.settings->effects[index];
                nodeName = pluginNodeName(name, ++pluginCount);
                const auto *info = catalog != nullptr ? catalog->find(e.type) : nullptr;
                if (info == nullptr && pluginFormat(e.type) != "lv2") {
                    // A CLAP or VST3 plugin is loaded from the file the
                    // catalog found it in: until a scan has, it waits (a
                    // finished scan reconciles again).
                    continue;
                }
                created = m_runner->want(nodeName,
                               PluginSpec{.type = e.type,
                                          .binary = info != nullptr ? info->binary : std::string(),
                                          .positions = positions,
                                          .sampleRate = rate,
                                          .enabled = e.enabled,
                                          .params = e.params,
                                          .description = "Patch Prime plugin: " + description +
                                                         (info != nullptr ? ": " + info->name : ""),
                                          .owner = m_owner,
                                          .title = (info != nullptr ? info->name : pluginUri(e.type)) +
                                                   " · " + description,
                                          .state = e.state});
                // A host (re)started just now: a node of that name in the
                // graph is the old one, on its way out.
                m_runner->setMetering(nodeName, m_effectMetering.contains(w.key));
                plugins[nodeName] = {w.key, index};
            } else {
                nodeName = effectsNodeName(name, ++runs);
                std::vector<EffectSettings> effects;
                for (const auto i : segment.indices) {
                    effects.push_back(w.settings->effects[i]);
                    keyed = keyed || effectTakesKey(w.settings->effects[i].type);
                }
                keep.insert(nodeName);
                const auto made = ensureEffectsNode(nodeName, w, std::move(effects), segment.indices,
                                                    positions, keyed, rate);
                if (!made) {
                    continue;
                }
                created = *made;
            }

            const auto node = filterNodes.find(nodeName);
            if (node == filterNodes.end()) {
                continue; // not up yet: left out
            }
            Part stage{.in = {}, .out = portsOf(node->second, "out", false), .key = {}};
            for (auto &port : portsOf(node->second, "in", false)) {
                (port.channel == "KEY" ? stage.key : stage.in).push_back(std::move(port));
            }
            ports.nodes.insert(node->second);
            for (const auto &p : stage.in) {
                ports.allIn.insert(p.portId);
            }
            for (const auto &p : stage.out) {
                ports.allOut.insert(p.portId);
            }
            // Not in the pass that made it, either: a node of that name in
            // the graph now is the one just replaced, on its way out.
            if (created || stage.in.size() != positions.size() ||
                stage.out.size() != positions.size() || stage.key.size() != (keyed ? 1U : 0U)) {
                continue;
            }
            stages.push_back(std::move(stage));
        }

        // The series: bus monitor -> first -> ... -> last (-> bus, on an input).
        std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
        const auto chain = [&](const std::vector<PortRef> &outputs, const std::vector<PortRef> &inputs) {
            for (const auto &[out, in] : pairPorts(outputs, inputs)) {
                desired.emplace(out.portId, in.portId);
            }
            link(outputs, inputs, existingLinks);
        };
        const auto busIn = portsOf(busId, "in", false);
        const auto busMonitor = portsOf(busId, "out", true);
        for (std::size_t i = 0; i + 1 < stages.size(); ++i) {
            chain(stages[i].out, stages[i + 1].in);
        }
        if (!stages.empty()) {
            if (effectsBeforeBus(w)) {
                chain(stages.back().out, busIn);
            } else {
                chain(busMonitor, stages.front().in);
            }
        }
        // Links between the bus and the series' nodes that the series no
        // longer has (a node that came up in between, or went).
        std::set<std::uint32_t> from = ports.allOut;
        for (const auto &p : busMonitor) {
            from.insert(p.portId);
        }
        std::set<std::uint32_t> into = ports.allIn;
        for (const auto &p : busIn) {
            into.insert(p.portId);
        }
        for (const auto &[pair, linkId] : existingLinks) {
            if (from.contains(pair.first) && into.contains(pair.second) &&
                !desired.contains(pair)) {
                m_graph->destroyObject(linkId);
            }
        }

        if (stages.empty()) {
            continue; // nothing up: the strip runs dry
        }
        ports.in = stages.front().in;
        ports.out = stages.back().out;
        for (const auto &stage : stages) {
            ports.key.insert(ports.key.end(), stage.key.begin(), stage.key.end());
        }
        up[name] = std::move(ports);
    }

    for (auto it = m_effects.begin(); it != m_effects.end();) {
        if (!keep.contains(it->first)) {
            qCInfo(lcBus, "removing effects node %s", it->first.c_str());
            m_graph->removeFilter(it->first);
            it = m_effects.erase(it);
        } else {
            ++it;
        }
    }
    std::set<std::string> pluginNodes;
    for (const auto &[node, at] : plugins) {
        pluginNodes.insert(node);
    }
    m_runner->prune(pluginNodes);
    m_plugins = std::move(plugins);
    return up;
}

void BusManager::linkCue(const std::map<std::string, std::uint32_t> &ready,
                         const std::map<std::string, Wanted> &wanted,
                         const std::map<std::string, std::vector<PortRef>> &stripOuts,
                         const LinkMap &existingLinks)
{
    const auto &cueKey = m_devices->store().cueOutput();
    const auto cue = std::ranges::find_if(
        wanted, [&cueKey](const auto &entry) { return entry.second.key == cueKey; });
    const bool usable = !cueKey.empty() && cue != wanted.end() && cue->second.hardwareNodeId &&
                        cue->second.settings->kind == EndpointKind::Hardware;
    const std::optional<std::uint32_t> hardwareNode =
        usable ? cue->second.hardwareNodeId : std::nullopt;

    // The ports a cue link can come from: every strip's output now, and every
    // bus's own outputs (a strip's output moves to its effects node when it
    // gets effects, and an old cue link from the bus must go then too).
    std::map<std::uint32_t, std::string> sourceOf; // port -> strip name
    for (const auto &[name, ports] : stripOuts) {
        for (const auto &p : ports) {
            sourceOf[p.portId] = name;
        }
    }
    for (const auto &[name, busId] : ready) {
        for (const auto &p : portsOf(busId, "out", true)) {
            sourceOf[p.portId] = name;
        }
    }

    // The links cue wants: each cued strip's output to the cue device.
    std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
    if (hardwareNode) {
        const auto inputs =
            selectPair(portsOf(*hardwareNode, "in", false), cue->second.settings->channelPair);
        for (const auto &[name, ports] : stripOuts) {
            if (name == cue->first || !m_devices->cued().contains(wanted.at(name).key)) {
                continue;
            }
            for (const auto &[out, in] : pairPorts(ports, inputs)) {
                desired.insert({out.portId, in.portId});
            }
            link(ports, inputs, existingLinks);
        }
    }

    // Links from strips into the cue device (now or before) that cue doesn't
    // want go, except each device's own strip's path to it.
    std::set<std::uint32_t> cleanNodes;
    if (hardwareNode) {
        cleanNodes.insert(*hardwareNode);
    }
    if (m_cueHardware && m_cueHardware != hardwareNode) {
        cleanNodes.insert(*m_cueHardware);
    }
    for (const auto node : cleanNodes) {
        std::set<std::uint32_t> inputs;
        for (const auto &p : portsOf(node, "in", false)) {
            inputs.insert(p.portId);
        }
        std::set<std::string> owners;
        for (const auto &[name, w] : wanted) {
            if (w.hardwareNodeId == node) {
                owners.insert(name);
            }
        }
        for (const auto &[pair, linkId] : existingLinks) {
            const auto source = sourceOf.find(pair.first);
            if (inputs.contains(pair.second) && source != sourceOf.end() &&
                !owners.contains(source->second) && !desired.contains(pair)) {
                m_graph->destroyObject(linkId);
            }
        }
    }
    m_cueHardware = hardwareNode;
}

void BusManager::linkKeys(const std::map<std::string, FxPorts> &fx,
                          const std::map<std::string, Wanted> &wanted,
                          const std::map<std::string, std::vector<PortRef>> &stripOuts,
                          const LinkMap &existingLinks)
{
    std::map<std::string, std::string> nameOf; // endpoint key -> bus name
    // Where each strip's sound goes: its sends, and (as they're accepted
    // below) the strips it keys. A key that closes a circle is refused.
    std::map<std::string, std::vector<std::string>> sendsOf;
    for (const auto &[name, w] : wanted) {
        nameOf[w.key] = name;
        sendsOf[w.key] = w.settings->sends;
    }
    for (const auto &[name, ports] : fx) {
        if (ports.key.empty()) {
            continue;
        }
        const auto &w = wanted.at(name);
        std::set<std::uint32_t> desired;
        for (const auto &e : w.settings->effects) {
            if (!effectTakesKey(e.type) || e.key.empty() || e.key == w.key) {
                continue;
            }
            if (reaches(sendsOf, w.key, e.key)) {
                qCWarning(lcBus, "%s can't be keyed by %s: it feeds it (a loop)", w.key.c_str(),
                          e.key.c_str()); // through sends or other keys
                continue;
            }
            const auto source = nameOf.find(e.key);
            if (source == nameOf.end()) {
                continue; // disabled or offline
            }
            const auto out = stripOuts.find(source->second);
            if (out == stripOuts.end()) {
                continue;
            }
            sendsOf[e.key].push_back(w.key); // the key strip now feeds this one
            for (const auto &port : out->second) {
                desired.insert(port.portId);
            }
            // Each built-in node in the series with a ducker has a key input.
            for (const auto &keyPort : ports.key) {
                link(out->second, {keyPort}, existingLinks);
            }
        }
        // Links into the key inputs from anything no ducker asks for go.
        for (const auto &keyPort : ports.key) {
            for (const auto &[pair, linkId] : existingLinks) {
                if (pair.second == keyPort.portId && !desired.contains(pair.first)) {
                    m_graph->destroyObject(linkId);
                }
            }
        }
    }
}

void BusManager::setTaps(std::map<std::string, std::string> recorders, std::string playerNode,
                         std::string playerTarget)
{
    m_recorders = std::move(recorders);
    m_playerNode = std::move(playerNode);
    m_playerTarget = std::move(playerTarget);
    scheduleReconcile();
}

std::vector<std::string> BusManager::stripPositions(const std::string &key) const
{
    for (const auto &[name, w] : wantedBuses()) {
        if (w.key == key) {
            return busPositions(w.endpoint);
        }
    }
    return {};
}

std::optional<std::uint32_t> BusManager::tapNode(const std::string &nodeName) const
{
    for (const auto &[id, node] : m_graph->nodes()) {
        if (prop(node, "node.name") == nodeName && prop(node, "patchprime.tap.owner") == m_owner) {
            return id;
        }
    }
    return std::nullopt;
}

std::vector<PortRef> BusManager::hardwarePortsOf(const Wanted &wanted) const
{
    if (!wanted.hardwareNodeId || wanted.settings->kind != EndpointKind::Hardware) {
        return {};
    }
    auto ports = portsOf(*wanted.hardwareNodeId, "out", false);
    if (ports.empty()) {
        ports = portsOf(*wanted.hardwareNodeId, "out", true); // a virtual source's
    }
    return selectPair(ports, wanted.settings->channelPair);
}

void BusManager::linkTaps(const std::map<std::string, std::uint32_t> &ready,
                          const std::map<std::string, Wanted> &wanted,
                          const std::map<std::string, FxPorts> &fx,
                          const std::map<std::string, std::vector<PortRef>> &stripOuts,
                          const LinkMap &existingLinks)
{
    if (m_recorders.empty() && m_playerNode.empty()) {
        return;
    }
    std::map<std::string, std::string> nameOf; // endpoint key -> bus name
    for (const auto &[name, w] : wanted) {
        nameOf[w.key] = name;
    }
    for (const auto &[nodeName, key] : m_recorders) {
        const auto node = tapNode(nodeName);
        const auto bus = nameOf.find(key);
        if (!node || bus == nameOf.end()) {
            continue;
        }
        const auto &w = wanted.at(bus->second);
        // A hardware input raw, as it comes in (summed to mono like the strip).
        std::vector<PortRef> from;
        if (effectsBeforeBus(w)) {
            from = hardwarePortsOf(w);
        } else if (const auto out = stripOuts.find(bus->second); out != stripOuts.end()) {
            from = out->second;
        }
        const auto into = portsOf(*node, "in", false);
        link(from, into, existingLinks);
        // Only from there: the strip's last node may have changed (a plugin
        // added at the end, or one that crashed).
        std::set<std::uint32_t> fromIds;
        for (const auto &p : from) {
            fromIds.insert(p.portId);
        }
        std::set<std::uint32_t> intoIds;
        for (const auto &p : into) {
            intoIds.insert(p.portId);
        }
        for (const auto &[pair, linkId] : existingLinks) {
            if (intoIds.contains(pair.second) && !fromIds.contains(pair.first)) {
                m_graph->destroyObject(linkId);
            }
        }
    }
    if (m_playerNode.empty()) {
        return;
    }
    const auto player = tapNode(m_playerNode);
    if (!player) {
        return;
    }
    // Into the target strip, and nowhere else: the target may have just
    // changed. A hardware input takes it where the device comes in: its
    // effects node, or its bus.
    std::vector<PortRef> into;
    if (const auto bus = nameOf.find(m_playerTarget); bus != nameOf.end()) {
        const auto &w = wanted.at(bus->second);
        if (const auto f = fx.find(bus->second); f != fx.end() && effectsBeforeBus(w)) {
            into = f->second.in;
        } else if (const auto it = ready.find(bus->second); it != ready.end()) {
            into = portsOf(it->second, "in", false);
        }
    }
    auto from = portsOf(*player, "out", false);
    if (into.size() == 1 && !from.empty()) {
        from.resize(1); // a mono strip: one channel, not both summed (6 dB louder)
    }
    link(from, into, existingLinks);
    const std::optional<std::uint32_t> target =
        into.empty() ? std::nullopt : std::optional(into.front().nodeId);
    std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
    for (const auto &[out, in] : pairPorts(from, into)) {
        desired.emplace(out.portId, in.portId);
    }
    for (const auto &[id, l] : m_graph->links()) {
        if (parseId(prop(l, "link.output.node")) != *player) {
            continue;
        }
        const std::pair ports{parseId(prop(l, "link.output.port")),
                              parseId(prop(l, "link.input.port"))};
        if (!target || !desired.contains(ports)) {
            m_graph->destroyObject(id);
        }
    }
}

void BusManager::linkSends(const std::map<std::string, std::uint32_t> &ready,
                           const std::map<std::string, FxPorts> &fx,
                           const std::map<std::string, std::vector<PortRef>> &stripOuts,
                           const std::map<std::string, Wanted> &wanted,
                           const std::map<std::string, WantedSend> &sends,
                           const std::map<std::string, std::uint32_t> &readySends,
                           const LinkMap &existingLinks)
{
    std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
    auto want = [&](const std::vector<PortRef> &outputs, const std::vector<PortRef> &inputs) {
        for (const auto &[out, in] : pairPorts(outputs, inputs)) {
            const std::pair ports{out.portId, in.portId};
            desired.insert(ports);
            if (!existingLinks.contains(ports) && !m_pendingLinks.contains(ports) &&
                m_graph->createLink(out.nodeId, out.portId, in.nodeId, in.portId, true)) {
                m_pendingLinks[ports] = Clock::now();
            }
        }
    };

    // Our nodes that send (input buses, mix buses, send nodes), and all of them.
    std::set<std::uint32_t> senders;
    std::set<std::uint32_t> ours;
    for (const auto &[name, id] : ready) {
        ours.insert(id);
        const auto &w = wanted.at(name);
        if (stageOf(w.endpoint.direction, w.settings->kind) != Stage::Output) {
            senders.insert(id);
            // The bus and its effects node both send (the bus fed the sends
            // before the strip had effects).
            for (const auto &port : stripOuts.at(name)) {
                senders.insert(port.nodeId);
            }
            // So does every node of its effects series when that comes after
            // the bus (one that was last before a plugin was added behind
            // it, say). Not on an input, where the series feeds the bus.
            if (const auto f = fx.find(name); f != fx.end() && !effectsBeforeBus(w)) {
                senders.insert(f->second.nodes.begin(), f->second.nodes.end());
            }
        }
    }
    for (const auto &[name, id] : readySends) {
        senders.insert(id);
        ours.insert(id);
        const auto &s = sends.at(name);
        const auto from = ready.find(s.from);
        const auto to = ready.find(s.to);
        if (from == ready.end() || to == ready.end()) {
            continue;
        }
        want(stripOuts.at(s.from), portsOf(id, "in", false));
        want(portsOf(id, "out", true), portsOf(to->second, "in", false));
    }

    // Links from one of our sending nodes into another of our nodes are ours
    // to manage: remove the ones no send asks for (direct links from before
    // send nodes, too). Links to anything else (an app recording an input
    // bus) are left alone.
    for (const auto &[id, l] : m_graph->links()) {
        const auto outNode = parseId(prop(l, "link.output.node"));
        const auto inNode = parseId(prop(l, "link.input.node"));
        if (!senders.contains(outNode) || !ours.contains(inNode)) {
            continue;
        }
        const std::pair ports{parseId(prop(l, "link.output.port")),
                              parseId(prop(l, "link.input.port"))};
        if (!desired.contains(ports)) {
            m_graph->destroyObject(id);
        }
    }
}

void BusManager::linkHardware(std::uint32_t busNodeId, const FxPorts *fx, const Wanted &wanted,
                              const LinkMap &existingLinks)
{
    if (!wanted.hardwareNodeId || wanted.settings->kind != EndpointKind::Hardware) {
        return; // offline (the bus stays, unlinked, as a stable target) or virtual
    }
    const auto hardware = *wanted.hardwareNodeId;
    const bool input = wanted.endpoint.direction == Direction::Input;
    // Every bus takes audio in on its "in" ports and gives it out on its
    // monitor ports (a virtual source's capture ports are flagged monitor too).
    auto hardwarePorts = input ? portsOf(hardware, "out", false) : portsOf(hardware, "in", false);
    if (input && hardwarePorts.empty()) {
        hardwarePorts =
            portsOf(hardware, "out", true); // a virtual source: its outputs are monitor ports
    }
    // With effects, the hardware links to the effects node instead (which
    // reconcileEffects() links to the bus).
    const auto ownPorts = input ? portsOf(busNodeId, "in", false) : portsOf(busNodeId, "out", true);
    auto busPorts = ownPorts;
    if (fx != nullptr) {
        busPorts = input ? fx->in : fx->out;
    }
    const auto used = selectPair(hardwarePorts, wanted.settings->channelPair);
    auto pairs = input ? pairPorts(used, busPorts) : pairPorts(busPorts, used);
    if (input && wanted.key == m_substitute) {
        pairs.clear(); // the player stands in for the device: its links go below
    }

    const auto now = Clock::now();
    std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
    for (const auto &[out, in] : pairs) {
        const std::pair ports{out.portId, in.portId};
        desired.insert(ports);
        if (existingLinks.contains(ports) || m_pendingLinks.contains(ports)) {
            continue;
        }
        if (m_graph->createLink(out.nodeId, out.portId, in.nodeId, in.portId, true)) {
            m_pendingLinks[ports] = now;
        }
    }

    // Links between the bus and its hardware that no longer fit (another
    // channel pair was chosen) go.
    std::set<std::uint32_t> hardwareIds;
    std::set<std::uint32_t> busIds;
    for (const auto &p : hardwarePorts) {
        hardwareIds.insert(p.portId);
    }
    for (const auto &p : busPorts) {
        busIds.insert(p.portId);
    }
    // The bus's own ports too, in use or not: when its effects node takes
    // over, the direct links go. (The effects node's links go with it.)
    for (const auto &p : ownPorts) {
        busIds.insert(p.portId);
    }
    // And every port of the effects series: the device moves to whichever
    // of its nodes comes first now.
    if (fx != nullptr) {
        busIds.insert(input ? fx->allIn.begin() : fx->allOut.begin(),
                      input ? fx->allIn.end() : fx->allOut.end());
    }
    for (const auto &[ports, linkId] : existingLinks) {
        const auto &[out, in] = ports;
        const bool between = input ? hardwareIds.contains(out) && busIds.contains(in)
                                   : busIds.contains(out) && hardwareIds.contains(in);
        if (between && !desired.contains(ports)) {
            m_graph->destroyObject(linkId);
        }
    }
}

} // namespace pp

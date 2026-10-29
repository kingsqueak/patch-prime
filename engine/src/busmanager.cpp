// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"

#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QLoggingCategory>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

Q_LOGGING_CATEGORY(lcBus, "patchprime.bus")

namespace pp {

namespace {

constexpr auto pendingTimeout = std::chrono::seconds(3);
const std::string nodeType = "PipeWire:Interface:Node";

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

Properties busProperties(const Endpoint &endpoint, const DeviceSettings &settings)
{
    const auto positions = busPositions(endpoint);
    const auto &name = settings.label.empty() ? endpoint.description : settings.label;
    const bool input = endpoint.direction == Direction::Input;
    return {
        {"factory.name", "support.null-audio-sink"},
        {"node.name", busNodeName(endpoint.direction, settings.busId)},
        {"node.description", (input ? "Patch Prime In: " : "Patch Prime Out: ") + name},
        {"media.class", "Audio/Sink"},
        {"audio.channels", std::to_string(positions.size())},
        {"audio.position", joinPositions(positions)},
        {"node.autoconnect", "false"},
        {"node.virtual", "true"},
        {"monitor.channel-volumes", "true"},
        {"object.linger", "true"},
        // Low priority so WirePlumber never picks a bus as the default device.
        {"priority.session", "0"},
        {"priority.driver", "0"},
        {"patchprime.key", endpoint.key},
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
    : QObject(parent), m_graph(graph), m_devices(devices)
{
    // Graph events come in bursts (a device brings a node and its ports);
    // reconcile once after the burst.
    m_reconcileTimer.setSingleShot(true);
    m_reconcileTimer.setInterval(20);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &BusManager::reconcile);

    connect(graph, &PipeWireGraph::objectAdded, this, &BusManager::scheduleReconcile);
    connect(graph, &PipeWireGraph::objectChanged, this, &BusManager::scheduleReconcile);
    connect(graph, &PipeWireGraph::objectRemoved, this, [this](const GraphObject &object) {
        m_applied.erase(object.id);
        scheduleReconcile();
    });
    connect(graph, &PipeWireGraph::connectedChanged, this, &BusManager::onConnectedChanged);
    connect(devices, &DeviceModel::settingsChanged, this, &BusManager::scheduleReconcile);
    scheduleReconcile();
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

void BusManager::removeAllBusesIn(PipeWireGraph &graph)
{
    for (const auto &[id, node] : graph.nodes()) {
        if (prop(node, "node.name").starts_with(busNamePrefix)) {
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
        out[busNodeName(row.endpoint.direction, settings->busId)] = std::move(w);
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

    std::map<std::string, std::uint32_t> existing;
    for (const auto &[id, node] : m_graph->nodes()) {
        const auto &name = prop(node, "node.name");
        if (name.starts_with(busNamePrefix)) {
            existing[name] = id;
        }
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> existingLinks;
    for (const auto &[id, link] : m_graph->links()) {
        const std::pair ports{parseId(prop(link, "link.output.port")),
                              parseId(prop(link, "link.input.port"))};
        existingLinks.insert(ports);
        m_pendingLinks.erase(ports);
    }

    // Buses nobody wants any more, including leftovers from a crash.
    for (const auto &[name, id] : existing) {
        if (!wanted.contains(name)) {
            qCInfo(lcBus, "removing bus %s", name.c_str());
            m_graph->destroyObject(id);
        }
    }

    for (const auto &[name, w] : wanted) {
        const auto it = existing.find(name);
        if (it == existing.end()) {
            if (!m_pendingBuses.contains(name)) {
                qCInfo(lcBus, "creating bus %s for %s", name.c_str(), w.key.c_str());
                if (m_graph->createNode(busProperties(w.endpoint, *w.settings))) {
                    m_pendingBuses[name] = now;
                }
            }
            continue;
        }
        m_pendingBuses.erase(name);
        const std::uint32_t busId = it->second;

        // Fader and mute: one volume per channel the bus actually has.
        std::size_t channels = busPositions(w.endpoint).size();
        if (const auto node = m_graph->nodes().find(busId); node != m_graph->nodes().end()) {
            if (const auto actual = parseId(prop(node->second, "audio.channels")); actual > 0) {
                channels = actual;
            }
        }
        const std::vector<float> volumes(
            channels, dbToLinear(w.settings->volumeDb, DeviceModel::minimumVolumeDb()));
        const std::pair state{volumes, w.settings->muted};
        if (auto applied = m_applied.find(busId);
            applied == m_applied.end() || applied->second != state) {
            if (m_graph->setNodeVolume(busId, volumes, w.settings->muted)) {
                m_applied[busId] = state;
            }
        }

        linkBus(busId, w, existingLinks);

        if (m_metering && !m_meters.contains(name)) {
            if (auto levels = m_graph->addMeter(name, static_cast<int>(channels))) {
                m_meters[name] = {w.key, std::move(levels)};
            }
        }
    }

    // Meters whose bus is gone or no longer wanted.
    for (auto it = m_meters.begin(); it != m_meters.end();) {
        if (!wanted.contains(it->first) || !existing.contains(it->first)) {
            m_graph->removeMeter(it->second.second);
            it = m_meters.erase(it);
        } else {
            ++it;
        }
    }
}

void BusManager::linkBus(std::uint32_t busNodeId, const Wanted &wanted,
                         const std::set<std::pair<std::uint32_t, std::uint32_t>> &existingLinks)
{
    if (!wanted.hardwareNodeId) {
        return; // hardware offline: the bus stays, unlinked, as a stable target
    }
    const auto hardware = *wanted.hardwareNodeId;
    const bool input = wanted.endpoint.direction == Direction::Input;
    const auto outputs = input ? portsOf(hardware, "out", false) : portsOf(busNodeId, "out", true);
    const auto inputs = input ? portsOf(busNodeId, "in", false) : portsOf(hardware, "in", false);

    const auto now = Clock::now();
    for (const auto &[out, in] : pairPorts(outputs, inputs)) {
        const std::pair ports{out.portId, in.portId};
        if (existingLinks.contains(ports) || m_pendingLinks.contains(ports)) {
            continue;
        }
        if (m_graph->createLink(out.nodeId, out.portId, in.nodeId, in.portId, true)) {
            m_pendingLinks[ports] = now;
        }
    }
}

} // namespace pp

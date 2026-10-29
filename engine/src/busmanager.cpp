// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "busmanager.h"

#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QCryptographicHash>
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

std::string virtualNodeName(const std::string &busId)
{
    return std::string(virtualNamePrefix) + busId;
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

std::string busOwnerFor(const QString &storePath)
{
    return QCryptographicHash::hash(storePath.toUtf8(), QCryptographicHash::Sha1)
        .toHex()
        .left(8)
        .toStdString();
}

std::string busNameFor(const Endpoint &endpoint, const DeviceSettings &settings)
{
    return settings.kind == EndpointKind::Hardware ? busNodeName(endpoint.direction, settings.busId)
                                                   : virtualNodeName(settings.busId);
}

std::string busMediaClass(const Endpoint &endpoint, const DeviceSettings &settings)
{
    switch (settings.kind) {
    case EndpointKind::VirtualSink:
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
        {"priority.driver", "0"},
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
      m_owner(busOwnerFor(devices->store().path()))
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

    std::map<std::string, std::uint32_t> existing;
    for (const auto &[id, node] : m_graph->nodes()) {
        const auto &name = prop(node, "node.name");
        const auto &owner = prop(node, "patchprime.owner");
        if ((name.starts_with(busNamePrefix) || name.starts_with(virtualNamePrefix)) &&
            (owner.empty() || owner == m_owner)) {
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
        if (wanted.contains(name)) {
            continue;
        }
        if (name == m_graph->defaultSinkName()) {
            const auto &key = prop(m_graph->nodes().at(id), "patchprime.key");
            const int row = m_devices->rowForKey(key);
            if (row >= 0 && m_devices->rows()[static_cast<std::size_t>(row)].nodeId) {
                m_graph->setConfiguredDefault(
                    Direction::Output,
                    m_devices->rows()[static_cast<std::size_t>(row)].endpoint.nodeName);
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
            if (!m_pendingBuses.contains(name)) {
                qCInfo(lcBus, "creating bus %s for %s", name.c_str(), w.key.c_str());
                if (m_graph->createNode(busProperties(w.endpoint, *w.settings, m_owner))) {
                    m_pendingBuses[name] = now;
                }
            }
            continue;
        }
        m_pendingBuses.erase(name);
        const std::uint32_t busId = it->second;
        const auto &node = m_graph->nodes().at(busId);

        // A bus made with different settings (an older version, a changed
        // channel count, a renamed virtual device) is replaced. WirePlumber
        // keeps it as the default if it was, since the name stays the same.
        const auto channels = busPositions(w.endpoint).size();
        const bool stale =
            prop(node, "media.class") != busMediaClass(w.endpoint, *w.settings) ||
            parseId(prop(node, "audio.channels")) != channels ||
            (w.settings->kind != EndpointKind::Hardware &&
             prop(node, "node.description") != busDescription(w.endpoint, *w.settings));
        if (stale) {
            qCInfo(lcBus, "replacing bus %s", name.c_str());
            m_graph->destroyObject(busId);
            continue;
        }
        ready[name] = busId;

        // Fader and mute.
        const std::vector<float> volumes(
            channels, dbToLinear(w.settings->volumeDb, DeviceModel::minimumVolumeDb()));
        const std::pair state{volumes, w.settings->muted};
        if (auto applied = m_applied.find(busId);
            applied == m_applied.end() || applied->second != state) {
            if (m_graph->setNodeVolume(busId, volumes, w.settings->muted)) {
                m_applied[busId] = state;
            }
        }

        linkHardware(busId, w, existingLinks);

        if (m_metering && !m_meters.contains(name)) {
            const bool sink = busMediaClass(w.endpoint, *w.settings) == "Audio/Sink";
            if (auto levels = m_graph->addMeter(name, static_cast<int>(channels), sink)) {
                m_meters[name] = {w.key, std::move(levels)};
            }
        }
    }

    linkSends(wanted, ready, existingLinks);

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

void BusManager::linkSends(const std::map<std::string, Wanted> &wanted,
                           const std::map<std::string, std::uint32_t> &ready,
                           const LinkMap &existingLinks)
{
    // Every bus's node id, by endpoint key.
    std::map<std::string, std::uint32_t> busByKey;
    for (const auto &[name, id] : ready) {
        busByKey[wanted.at(name).key] = id;
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
    std::set<std::uint32_t> inputBuses;
    std::set<std::uint32_t> allBuses;
    for (const auto &[name, id] : ready) {
        allBuses.insert(id);
        const auto &w = wanted.at(name);
        if (w.endpoint.direction != Direction::Input) {
            continue;
        }
        inputBuses.insert(id);
        for (const auto &destination : w.settings->sends) {
            const auto target = busByKey.find(destination);
            if (target == busByKey.end()) {
                continue; // destination disabled, offline-and-unmade, or forgotten
            }
            for (const auto &[out, in] :
                 pairPorts(portsOf(id, "out", true), portsOf(target->second, "in", false))) {
                desired.emplace(out.portId, in.portId);
                if (!existingLinks.contains({out.portId, in.portId}) &&
                    !m_pendingLinks.contains({out.portId, in.portId}) &&
                    m_graph->createLink(out.nodeId, out.portId, in.nodeId, in.portId, true)) {
                    m_pendingLinks[{out.portId, in.portId}] = Clock::now();
                }
            }
        }
    }

    // Links from one of our input buses into another of our buses are ours to
    // manage: remove the ones no send asks for. Links to anything else (an
    // app recording an input bus) are left alone.
    for (const auto &[id, link] : m_graph->links()) {
        const auto outNode = parseId(prop(link, "link.output.node"));
        const auto inNode = parseId(prop(link, "link.input.node"));
        if (!inputBuses.contains(outNode) || !allBuses.contains(inNode)) {
            continue;
        }
        const std::pair ports{parseId(prop(link, "link.output.port")),
                              parseId(prop(link, "link.input.port"))};
        if (!desired.contains(ports)) {
            m_graph->destroyObject(id);
        }
    }
}

void BusManager::linkHardware(std::uint32_t busNodeId, const Wanted &wanted,
                              const LinkMap &existingLinks)
{
    if (!wanted.hardwareNodeId || wanted.settings->kind != EndpointKind::Hardware) {
        return; // offline (the bus stays, unlinked, as a stable target) or virtual
    }
    const auto hardware = *wanted.hardwareNodeId;
    const bool input = wanted.endpoint.direction == Direction::Input;
    // Every bus takes audio in on its "in" ports and gives it out on its
    // monitor ports (a virtual source's capture ports are flagged monitor too).
    auto outputs = input ? portsOf(hardware, "out", false) : portsOf(busNodeId, "out", true);
    if (input && outputs.empty()) {
        outputs = portsOf(hardware, "out", true); // a virtual source: its outputs are monitor ports
    }
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

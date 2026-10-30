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

std::string busNameFor(const Endpoint &endpoint, const DeviceSettings &settings)
{
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
        if (const auto &name = prop(node, "node.name");
            name.starts_with(busNamePrefix) || name.starts_with(sendNamePrefix)) {
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
        if ((name.starts_with(busNamePrefix) || name.starts_with(virtualNamePrefix) ||
             name.starts_with(sendNamePrefix)) &&
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

        applyVolume(busId, channels, w.settings->volumeDb, w.settings->muted); // fader and mute

        linkHardware(busId, w, existingLinks);

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

    linkSends(ready, wanted, sends, readySends, existingLinks);

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

void BusManager::linkSends(const std::map<std::string, std::uint32_t> &ready,
                           const std::map<std::string, Wanted> &wanted,
                           const std::map<std::string, WantedSend> &sends,
                           const std::map<std::string, std::uint32_t> &readySends,
                           const LinkMap &existingLinks)
{
    std::set<std::pair<std::uint32_t, std::uint32_t>> desired;
    auto link = [&](const std::vector<PortRef> &outputs, const std::vector<PortRef> &inputs) {
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
        link(portsOf(from->second, "out", true), portsOf(id, "in", false));
        link(portsOf(id, "out", true), portsOf(to->second, "in", false));
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
    auto hardwarePorts = input ? portsOf(hardware, "out", false) : portsOf(hardware, "in", false);
    if (input && hardwarePorts.empty()) {
        hardwarePorts =
            portsOf(hardware, "out", true); // a virtual source: its outputs are monitor ports
    }
    const auto busPorts = input ? portsOf(busNodeId, "in", false) : portsOf(busNodeId, "out", true);
    const auto used = selectPair(hardwarePorts, wanted.settings->channelPair);
    const auto pairs = input ? pairPorts(used, busPorts) : pairPorts(busPorts, used);

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

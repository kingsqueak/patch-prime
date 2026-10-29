// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "endpoint.h"
#include "graphtypes.h"

#include <QObject>
#include <QTimer>

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pp {

class DeviceModel;
class PipeWireGraph;
struct MeterLevels;

// --- Pure helpers (unit tested) ---------------------------------------------

constexpr std::string_view busNamePrefix = "patchprime.bus.";

constexpr std::string_view virtualNamePrefix = "patchprime.virtual.";

// "patchprime.bus.in.<busId>" or "patchprime.bus.out.<busId>"
std::string busNodeName(Direction direction, const std::string &busId);
// "patchprime.virtual.<busId>": a virtual device is its own bus.
std::string virtualNodeName(const std::string &busId);

// dB to linear amplitude; at or below minimumDb the result is 0 (silent).
float dbToLinear(double db, double minimumDb);

// audio.position for a bus: the endpoint's own positions when known, else a
// sensible default for the channel count (MONO, FL FR, AUX0..).
std::vector<std::string> busPositions(const Endpoint &endpoint);

// The node props for an endpoint's bus: a null sink that never links itself,
// lingers after the daemon exits, and applies its volume to its monitor.
// `owner` tags the bus with the settings it belongs to (see BusManager).
Properties busProperties(const Endpoint &endpoint, const DeviceSettings &settings,
                         const std::string &owner = {});

// The node that is an endpoint's bus: patchprime.bus.* for hardware,
// patchprime.virtual.* for a virtual device (which is its own bus).
std::string busNameFor(const Endpoint &endpoint, const DeviceSettings &settings);
std::string busMediaClass(const Endpoint &endpoint, const DeviceSettings &settings);
std::string busDescription(const Endpoint &endpoint, const DeviceSettings &settings);

// A short tag for a settings file, so engines with different settings (the
// daemon and a test, say) leave each other's buses alone.
std::string busOwnerFor(const QString &storePath);

struct PortRef
{
    std::uint32_t nodeId = 0;
    std::uint32_t portId = 0;
    std::string channel; // audio.channel, e.g. "FL"
};

// The ports of one stereo pair (1 = the first two, 2 = the next two, ...), or
// all of them for pair 0 or a pair the ports don't have.
std::vector<PortRef> selectPair(const std::vector<PortRef> &ports, int pair);

// Which output port feeds which input port. Ports pair up by channel name
// when both sides have the same set of names, otherwise in order. A single
// output feeds every input (mono to stereo) and every output feeds a single
// input (stereo to mono).
std::vector<std::pair<PortRef, PortRef>> pairPorts(const std::vector<PortRef> &outputs,
                                                   const std::vector<PortRef> &inputs);

// --- BusManager --------------------------------------------------------------

// Keeps the PipeWire graph in line with the saved settings:
//
//   input    hardware source ──links──► [bus] ──monitor──► sends
//   mix bus  sends ──► [virtual sink] ──monitor──► sends
//   output   sends ──► [bus] ──monitor──links──► hardware sink
//
// One bus per enabled endpoint, named from its busId so it survives the
// endpoint being recognised under a new node.name. The fader and mute are the
// bus's channelVolumes and mute; the hardware volume is never touched. Buses
// and their links linger, so audio keeps flowing if the daemon exits. On every
// graph or settings change the manager compares what exists with what should
// and fixes the difference; nothing is relinked on a timer. It only removes
// buses tagged with its own owner, or untagged ones (from older versions).
class BusManager : public QObject
{
    Q_OBJECT

public:
    BusManager(PipeWireGraph *graph, DeviceModel *devices, QObject *parent = nullptr);

    // Meter streams cost a little CPU, so they only run while someone watches.
    void setMetering(bool enabled);
    [[nodiscard]] bool isMetering() const { return m_metering; }

    // Peaks per endpoint key since the last call (linear, per channel).
    std::map<std::string, std::vector<float>> takeLevels();

    // Removes every Patch Prime bus (and so its links) in the graph. Used by
    // `patchprimed --remove-buses`, which runs without a BusManager.
    static void removeAllBusesIn(PipeWireGraph &graph);

private:
    struct Wanted
    {
        std::string key;
        Endpoint endpoint;
        const DeviceSettings *settings = nullptr;
        std::optional<std::uint32_t> hardwareNodeId;
    };

    void scheduleReconcile();
    void reconcile();
    void onConnectedChanged(bool connected);
    [[nodiscard]] std::map<std::string, Wanted> wantedBuses() const;
    using LinkMap = std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>;
    void linkHardware(std::uint32_t busNodeId, const Wanted &wanted, const LinkMap &existingLinks);
    void linkSends(const std::map<std::string, Wanted> &wanted,
                   const std::map<std::string, std::uint32_t> &ready, const LinkMap &existingLinks);
    [[nodiscard]] std::vector<PortRef> portsOf(std::uint32_t nodeId, const std::string &direction,
                                               bool monitor) const;

    using Clock = std::chrono::steady_clock;

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    std::string m_owner;
    QTimer m_reconcileTimer;
    bool m_metering = false;

    // Requests sent but not yet seen in the graph, so a reconcile in between
    // doesn't ask twice. They expire in case PipeWire refused silently.
    std::map<std::string, Clock::time_point> m_pendingBuses;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Clock::time_point> m_pendingLinks;
    // Last volume and mute sent to each bus node, so unchanged values aren't resent.
    std::map<std::uint32_t, std::pair<std::vector<float>, bool>> m_applied;
    // Meter per bus node name, and the endpoint key it reports for.
    std::map<std::string, std::pair<std::string, std::shared_ptr<MeterLevels>>> m_meters;
};

} // namespace pp

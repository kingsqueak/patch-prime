// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "effects.h"
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

constexpr std::string_view sendNamePrefix = "patchprime.send.";

// The media class of a send's gain node: one of Patch Prime's own, so Plasma,
// pactl and WirePlumber don't list it as a device or link it.
constexpr std::string_view sendMediaClass = "Audio/Patchprime/Send";

// The old id-based names, still used by entries without a busName:
// "patchprime.bus.in.<busId>" or "patchprime.bus.out.<busId>",
// and "patchprime.virtual.<busId>" (a virtual device is its own bus).
std::string busNodeName(Direction direction, const std::string &busId);
std::string virtualNodeName(const std::string &busId);

// A label as a node-name part: lowercase letters, digits and dashes
// ("Maudio 1" -> "maudio-1"); "device" when nothing is left.
std::string nameSlug(const std::string &label);

// A readable, unused bus name for the entry under key, from its label (or
// description): "patchprime.in.<slug>" / "patchprime.out.<slug>" for a
// hardware endpoint, "patchprime.<slug>" for a virtual device; "-2", "-3"...
// when taken.
std::string readableBusName(const DeviceStore &store, const std::string &key);

// Gives every entry without a busName a readable one (and a virtual device
// its matching nodeName). Returns old name -> new name, so the defaults and
// app routes that used the old names can follow.
std::map<std::string, std::string> migrateBusNames(DeviceStore &store);

// The media class of a strip's effects node (see BusManager): hidden, like sends.
constexpr std::string_view effectsMediaClass = "Audio/Patchprime/Effects";

// The effects node for the bus named busName: "patchprime.fx.<rest>"
// ("patchprime.out.maudio-1" -> "patchprime.fx.out.maudio-1").
std::string effectsNodeName(const std::string &busName);

// Whether audio from the strip `from` reaches the strip `to` through sends
// (directly or via other strips), by endpoint key: a ducker on `from` keyed
// by `to` would then be a loop. sends: each strip's send destinations.
bool reaches(const std::map<std::string, std::vector<std::string>> &sends,
             const std::string &from, const std::string &to);

// Builds a chain running effects, prepared for sampleRate and channels.
// Effects of unknown types are skipped.
std::unique_ptr<EffectChain> buildChain(const std::vector<EffectSettings> &effects,
                                        double sampleRate, int channels);
// Applies effects to chain in place when it holds the same types in the same
// order (so levels glide and bypass crossfades instead of starting afresh);
// false when the chain has to be rebuilt.
bool updateChain(EffectChain &chain, const std::vector<EffectSettings> &effects);

// "patchprime.send.<from busId>.<to busId>"
std::string sendNodeName(const std::string &fromBusId, const std::string &toBusId);

// A send's gain node: a null sink with the sending bus's channel layout,
// between the two buses; its volume is the send level.
Properties sendProperties(const std::string &name, const std::vector<std::string> &positions,
                          const std::string &description, const std::string &owner);

// dB to linear amplitude; at or below minimumDb the result is 0 (silent).
float dbToLinear(double db, double minimumDb);

// audio.position for a bus: the endpoint's own positions when known, else a
// sensible default for the channel count (MONO, FL FR, AUX0..).
std::vector<std::string> busPositions(const Endpoint &endpoint);

// The node props for an endpoint's bus: a null sink that never links itself,
// lingers after the daemon exits, and applies its volume to its monitor.
// `owner` tags the bus with the settings it belongs to (see BusManager).
// Buses have a driver priority above any hardware (ALSA devices use about
// 1000-2600), so one of them clocks the graph, in both clock modes: a chosen
// master interface gets more (clockDevicePriority() in audiosetup.h) and
// wins while it's plugged in; when it's unplugged, the buses take over by
// themselves, and hand back when it returns.
constexpr int busDriverPriority = 20000;

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

// How often one node may be created. A node that's asked for again and again
// without ever appearing (a bug, or PipeWire refusing) would otherwise be
// created on every reconcile; each costs PipeWire file descriptors, and once
// it runs out, all audio stops (seen 2026-10-29). At most maxPerWindow
// attempts per window, counted afresh once the node appears.
class CreationLimiter
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int maxPerWindow = 5;
    static constexpr std::chrono::seconds window{60};

    enum class Decision : std::uint8_t
    {
        Create,
        Refuse,      // over the limit
        RefuseFirst, // over the limit, for the first time this window (worth a warning)
    };

    // Counts an attempt to create `name` now.
    Decision attempt(const std::string &name, Clock::time_point now);
    // The node appeared: count afresh next time.
    void appeared(const std::string &name) { m_attempts.erase(name); }

private:
    struct Attempts
    {
        Clock::time_point first;
        int count = 0;
    };
    std::map<std::string, Attempts> m_attempts;
};

// --- BusManager --------------------------------------------------------------

// Keeps the PipeWire graph in line with the saved settings:
//
//   input    hardware source ──links──► [bus] ──monitor──► sends
//   mix bus  sends ──► [virtual sink] ──monitor──► sends
//   output   sends ──► [bus] ──monitor──links──► hardware sink
//   send     [bus] ──monitor──► [send node] ──monitor──► [bus]
//   cue      (any strip's output) ──links──► the cue output's hardware,
//            while that output's own bus is muted
//   effects  hardware source ──► [fx filter] ──► [bus]           (inputs)
//            [bus] ──monitor──► [fx filter] ──► hardware, sends  (the rest)
//
// A strip with effects gets a processing node (patchprime.fx.*, a pw_filter
// in this process running the chain on the realtime thread). On a hardware
// input it sits in front of the bus, so apps recording the bus (the system
// mic) and the sends get the processed sound, and the fader comes after the
// effects, as on a console. Elsewhere it sits after the bus, so the fader
// comes first (right for a limiter on an output); apps recording a virtual
// device's monitor get it without the effects. Meters show the bus. Unlike
// buses, filters don't linger: while the daemon is down, a strip with effects
// is silent; before its filter is up, it runs dry.
//
// Each send has its own small gain node (patchprime.send.*), whose volume
// is the send level, so one input can go to two places at different levels.
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

    // Old bus names -> new ones (see migrateBusNames): when an old-named bus
    // is removed while it's a system default, the default moves to its new
    // name instead of back to the hardware.
    void setRenamed(std::map<std::string, std::string> renamed) { m_renamed = std::move(renamed); }
    [[nodiscard]] bool isMetering() const { return m_metering; }

    // Peaks per endpoint key since the last call (linear, per channel).
    std::map<std::string, std::vector<float>> takeLevels();
    // RMS per channel (linear) and short-term loudness (LUFS) now, per key.
    struct Loudness
    {
        std::vector<float> rms;
        float lufs = -70.0F;
    };
    [[nodiscard]] std::map<std::string, Loudness> loudness() const;
    // Effects' own meters since the last call (see Effect::takeMeters), by
    // endpoint key and effect index; only effects that have meters.
    std::map<std::pair<std::string, std::size_t>, std::vector<float>> takeEffectMeters();

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

    // A send's gain node, between the bus node names `from` and `to`.
    struct WantedSend
    {
        std::string from;
        std::string to;
        std::vector<std::string> positions;
        double levelDb = 0.0;
        std::string description;
    };

    void scheduleReconcile();
    void reconcile();
    void onConnectedChanged(bool connected);
    [[nodiscard]] std::map<std::string, Wanted> wantedBuses() const;
    [[nodiscard]] static std::map<std::string, WantedSend>
    wantedSends(const std::map<std::string, Wanted> &buses);
    void applyVolume(std::uint32_t nodeId, std::size_t channels, double db, bool muted);
    // Asks the limiter; warns the first time a node is refused.
    bool mayCreate(const std::string &name, CreationLimiter::Clock::time_point now);
    using LinkMap = std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>;
    // A strip's processing node, by effects node name.
    struct Fx
    {
        std::string key; // the strip's endpoint key
        std::shared_ptr<Processor> processor;
        std::vector<std::string> positions;
        bool keyed = false;                  // has the key input
        std::vector<EffectSettings> effects; // what the chain was last given
        int sampleRate = 0;
    };

    struct FxPorts
    {
        std::vector<PortRef> in;
        std::vector<PortRef> out;
        std::vector<PortRef> key; // the key input, when the strip has a ducker
    };
    // A hardware input: its effects go in front of its bus.
    [[nodiscard]] static bool effectsBeforeBus(const Wanted &wanted);
    // Makes, updates and removes effects nodes for the ready buses, and links
    // each to its bus; returns the ports of those that are up, by bus name.
    std::map<std::string, FxPorts> reconcileEffects(const std::map<std::string, std::uint32_t> &ready,
                                                    const std::map<std::string, Wanted> &wanted,
                                                    const LinkMap &existingLinks);
    // Links a hardware strip to its bus, or through fx when it has effects.
    void linkHardware(std::uint32_t busNodeId, const FxPorts *fx, const Wanted &wanted,
                      const LinkMap &existingLinks);
    void link(const std::vector<PortRef> &outputs, const std::vector<PortRef> &inputs,
              const LinkMap &existingLinks);
    // Cue: links each cued strip's output straight to the cue output's
    // hardware (its bus is muted meanwhile, see reconcile()), and removes
    // cue links that no longer apply.
    void linkCue(const std::map<std::string, Wanted> &wanted,
                 const std::map<std::string, std::vector<PortRef>> &stripOuts,
                 const LinkMap &existingLinks);
    // Feeds each effects node's key input from the strips its duckers
    // listen to (their outputs, summed), and removes other links into it.
    void linkKeys(const std::map<std::string, FxPorts> &fx,
                  const std::map<std::string, Wanted> &wanted,
                  const std::map<std::string, std::vector<PortRef>> &stripOuts,
                  const LinkMap &existingLinks);
    void linkSends(const std::map<std::string, std::uint32_t> &ready,
                   const std::map<std::string, std::vector<PortRef>> &stripOuts,
                   const std::map<std::string, Wanted> &wanted,
                   const std::map<std::string, WantedSend> &sends,
                   const std::map<std::string, std::uint32_t> &readySends,
                   const LinkMap &existingLinks);
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
    CreationLimiter m_limiter;
    std::map<std::string, std::string> m_renamed;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Clock::time_point> m_pendingLinks;
    // Last volume and mute sent to each bus node, so unchanged values aren't resent.
    std::map<std::uint32_t, std::pair<std::vector<float>, bool>> m_applied;
    // Meter per bus node name, and the endpoint key it reports for.
    std::map<std::string, std::pair<std::string, std::shared_ptr<MeterLevels>>> m_meters;
    std::map<std::string, Fx> m_effects;
    // The hardware node cue last played to, so its links can be cleaned up
    // when cue moves elsewhere or is turned off.
    std::optional<std::uint32_t> m_cueHardware;
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "endpoint.h"

#include <QDateTime>
#include <QString>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

// Hardware endpoints come from PipeWire; virtual devices are made by Patch
// Prime itself and exist only as long as their saved entry does.
enum class EndpointKind : std::uint8_t
{
    Hardware,      // any endpoint PipeWire reports (cards, other apps' virtual devices)
    VirtualSink,   // apps play into it, others record its monitor ("Stream Mix")
    VirtualSource, // apps record from it, like a microphone ("Chat Mic")
    VirtualInput,  // apps play into it, and it's routed like an input ("Desktop", "Music")
};

// "hardware", "virtual-sink", "virtual-source", "virtual-input" (in the settings
// file and the D-Bus state).
std::string kindName(EndpointKind kind);
// The reverse; anything unknown is Hardware.
EndpointKind kindFromName(std::string_view name);

// Where a strip sits in the console. Sends only go to a later stage
// (input -> bus, input -> output, bus -> output), so routing can't loop.
enum class Stage : std::uint8_t
{
    Input,  // inputs and virtual inputs: they feed sends
    Bus,    // mix buses (virtual sinks): fed by inputs, they feed outputs
    Output, // hardware outputs and virtual microphones: the end of the line
};

Stage stageOf(Direction direction, EndpointKind kind);

// What clocks the audio graph (the PipeWire "driver").
enum class ClockMode : std::uint8_t
{
    // Patch Prime's own buses: unplugging any interface doesn't stall the
    // rest. Hardware follows with PipeWire's rate matching.
    Internal,
    // A hardware device, as PipeWire picks it: the lowest latency, but
    // unplugging that device stalls everything linked to it for a moment.
    Hardware,
};

// "internal" / "hardware"; anything unknown is Internal.
std::string clockModeName(ClockMode mode);
ClockMode clockModeFromName(std::string_view name);
// "input", "bus", "output" (in the D-Bus state).
std::string stageName(Stage stage);

// What the user chose for one endpoint, kept across restarts and replugs.
struct DeviceSettings
{
    EndpointKind kind = EndpointKind::Hardware;
    // Permanent random id, made when the entry is created. It names the
    // endpoint's bus, so the bus keeps its name when the endpoint is recognised
    // under a new node.name.
    std::string busId;
    bool enabled = false;
    std::string label; // empty: show the system description
    double volumeDb = 0.0;
    bool muted = false;
    // Inputs and mix buses: the keys of the strips this one is sent to.
    std::vector<std::string> sends;
    // The level of each send in dB, by destination key; 0 dB when missing.
    std::map<std::string, double> sendLevels;
    // Inputs only: mix every hardware channel into a one-channel bus, so a mic
    // on one input of a stereo interface is heard in both ears.
    bool mono = false;
    // Hardware with more than two channels (a card in pro-audio mode): 0 uses
    // every channel; n uses only the stereo pair 2n-1 and 2n (1 = channels
    // 1-2, 2 = 3-4, ...), and the bus is stereo.
    int channelPair = 0;
    // Last seen details, so an offline endpoint can still be shown and matched.
    std::string nodeName;
    std::string mediaClass;
    Direction direction = Direction::Output;
    bool isVirtual = false;
    std::string description;
    int channels = 0;
    std::vector<std::string> positions;
    Signature signature;
    QDateTime lastSeen;
};

// ~/.config/patchprime/devices.json, keyed by Endpoint::key. Saves atomically.
// Only endpoints the user has touched (enabled or labelled) are stored.
class DeviceStore
{
public:
    // An empty path means the standard location under ~/.config/patchprime.
    explicit DeviceStore(QString path = {});

    [[nodiscard]] const QString &path() const { return m_path; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    // Returns false and sets lastError() when the file exists but can't be
    // read. A missing file is an empty store.
    bool load();
    bool save();

    [[nodiscard]] ClockMode clock() const { return m_clock; }
    void setClock(ClockMode clock) { m_clock = clock; }

    [[nodiscard]] const std::map<std::string, DeviceSettings> &entries() const { return m_entries; }
    [[nodiscard]] const DeviceSettings *find(const std::string &key) const;
    // Creates the entry (with a new busId) when it doesn't exist.
    DeviceSettings &upsert(const std::string &key);
    // Also removes the sends that pointed at it.
    bool remove(const std::string &key);
    // Moves an entry to a new key when its endpoint is recognised under a new
    // name, and updates every send that pointed at the old key.
    bool rekey(const std::string &from, const std::string &to);

    static QString defaultPath();
    static std::string newBusId();
    // Key for a virtual device Patch Prime makes: "patchprime-virtual|<busId>".
    static std::string virtualKey(const std::string &busId);

private:
    QString m_path;
    QString m_lastError;
    ClockMode m_clock = ClockMode::Internal;
    std::map<std::string, DeviceSettings> m_entries;
};

} // namespace pp

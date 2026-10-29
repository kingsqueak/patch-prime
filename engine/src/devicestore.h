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
    // Inputs only: the keys of the outputs and virtual devices this input is
    // sent to.
    std::vector<std::string> sends;
    // Inputs only: mix every hardware channel into a one-channel bus, so a mic
    // on one input of a stereo interface is heard in both ears.
    bool mono = false;
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
    std::map<std::string, DeviceSettings> m_entries;
};

} // namespace pp

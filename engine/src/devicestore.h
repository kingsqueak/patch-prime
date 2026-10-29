// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "endpoint.h"

#include <QDateTime>
#include <QString>

#include <map>
#include <string>
#include <vector>

namespace pp {

// What the user chose for one endpoint, kept across restarts and replugs.
struct DeviceSettings
{
    // Permanent random id, made when the entry is created. It names the
    // endpoint's bus, so the bus keeps its name when the endpoint is recognised
    // under a new node.name.
    std::string busId;
    bool enabled = false;
    std::string label; // empty: show the system description
    double volumeDb = 0.0;
    bool muted = false;
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
    bool remove(const std::string &key);
    // Moves an entry to a new key when its endpoint is recognised under a new name.
    bool rekey(const std::string &from, const std::string &to);

    static QString defaultPath();
    static std::string newBusId();

private:
    QString m_path;
    QString m_lastError;
    std::map<std::string, DeviceSettings> m_entries;
};

} // namespace pp

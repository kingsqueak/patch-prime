// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QDateTime>
#include <QString>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace pp {

enum class StreamDirection : std::uint8_t
{
    Playback, // Stream/Output/Audio: the app plays into a sink
    Record,   // Stream/Input/Audio: the app records from a source (or a sink's monitor)
};

// "playback" / "record" (in the settings file and the D-Bus state).
std::string streamDirectionName(StreamDirection direction);
StreamDirection streamDirectionFromName(std::string_view name);

// Where one application's streams go, kept across app restarts and reboots.
struct AppSettings
{
    StreamDirection direction = StreamDirection::Playback;
    std::string name; // application.name, for display
    std::string icon; // application.icon-name
    // node.name of the device or bus the app's streams link to; empty means
    // the system default.
    std::string target;
    QDateTime lastSeen;
    // Left out of the mixer's app strips.
    bool hidden = false;
};

// ~/.config/patchprime/apps.json, keyed by app key ("playback|<binary>").
// Saves atomically.
class AppStore
{
public:
    // An empty path means the standard location under ~/.config/patchprime.
    explicit AppStore(QString path = {});

    [[nodiscard]] const QString &path() const { return m_path; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    // Returns false and sets lastError() when the file exists but can't be
    // read. A missing file is an empty store.
    bool load();
    bool save();

    [[nodiscard]] const std::map<std::string, AppSettings> &entries() const { return m_entries; }
    [[nodiscard]] const AppSettings *find(const std::string &key) const;
    AppSettings &upsert(const std::string &key) { return m_entries[key]; }
    bool remove(const std::string &key) { return m_entries.erase(key) > 0; }

    static QString defaultPath();

private:
    QString m_path;
    QString m_lastError;
    std::map<std::string, AppSettings> m_entries;
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "midimapping.h"
#include "mixertargets.h"

#include <QList>
#include <QObject>

#include <map>
#include <optional>
#include <string>

namespace pp {

class DeviceModel;
class RecordingManager;

// What a hotkey does to its target.
enum class HotkeyMode : std::uint8_t
{
    Toggle,     // an on/off target: mute, cue, a send, an effect, recording; or a scene
    PushToTalk, // Mute target: unmuted while held
    PushToMute, // Mute target: muted while held
    Duck,       // Volume target: lowered (Hotkeys::duckDb) while held
    LevelUp,    // Volume or SendLevel target: up a step (and on while held)
    LevelDown,
};
std::string hotkeyModeName(HotkeyMode mode); // "toggle", "pushToTalk", ...

struct Hotkey
{
    std::string id; // the shortcut's name in the desktop's shortcuts
    HotkeyMode mode = HotkeyMode::Toggle;
    MidiTarget target;
    bool operator==(const Hotkey &) const = default;
};

QJsonObject hotkeyToJson(const Hotkey &hotkey);
// nullopt unless the mode suits the target (push to talk on a mute, ...).
std::optional<Hotkey> hotkeyFromJson(const QJsonObject &json);

// Where the keys are: the desktop's global shortcuts (kglobalaccel on
// Plasma; KGlobalAccelShortcuts in the daemon), or the tests' stand-in. It
// keeps the keys themselves, so they can also be changed in System
// Settings. Keys are Qt key combinations (QKeySequence's first key, as int).
class ShortcutBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    [[nodiscard]] virtual bool isAvailable() const = 0;
    // Makes (or renames) a shortcut; its saved keys come with keysChanged.
    virtual void add(const std::string &id, const std::string &name) = 0;
    virtual void remove(const std::string &id) = 0;
    virtual void setKeys(const std::string &id, const QList<int> &keys) = 0;
    [[nodiscard]] virtual QList<int> keys(const std::string &id) const = 0;

Q_SIGNALS:
    void pressed(const QString &id);
    void repeated(const QString &id); // held down (the key repeats)
    void released(const QString &id);
    void keysChanged(const QString &id);
    void availableChanged();
};

// Global hotkeys: shortcuts that work in any application, saved with the
// devices (DeviceStore::hotkeys), their keys in a ShortcutBackend.
class Hotkeys : public QObject
{
    Q_OBJECT

public:
    Hotkeys(DeviceModel *devices, ShortcutBackend *backend, QObject *parent = nullptr);

    void setRecordings(RecordingManager *recordings) { m_targets.setRecordings(recordings); }

    // A new hotkey (no keys yet); its id, or empty when the mode doesn't
    // suit the target.
    std::string add(HotkeyMode mode, const MidiTarget &target);
    void remove(const std::string &id);
    void setKeys(const std::string &id, const QList<int> &keys);
    [[nodiscard]] QList<int> keys(const std::string &id) const { return m_backend->keys(id); }
    // Whether the desktop has global shortcuts (kglobalaccel) at all.
    [[nodiscard]] bool isAvailable() const { return m_backend->isAvailable(); }
    // For people: "Push to talk: Mic", "Mic: mute", "Mic: level up".
    [[nodiscard]] std::string describe(const Hotkey &hotkey) const;

    static constexpr double duckDb = 15.0;
    static constexpr double stepDb = 1.0;

Q_SIGNALS:
    void sceneRequested(const QString &name);
    // The list or a hotkey's keys changed.
    void changed();

private:
    void sync();
    void press(const QString &id, bool repeat);
    void release(const QString &id);
    [[nodiscard]] std::optional<Hotkey> find(const QString &id) const;

    DeviceModel *m_devices;
    ShortcutBackend *m_backend;
    MixerTargets m_targets;
    std::map<std::string, std::string> m_registered; // id -> the name it has there
    // Ducks held now: id -> {the level before, the level it ducked to}.
    std::map<std::string, std::pair<double, double>> m_ducked;
};

} // namespace pp

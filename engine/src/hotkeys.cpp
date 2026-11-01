// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "hotkeys.h"

#include "devicemodel.h"
#include "soundboard.h"

#include <QUuid>

#include <array>
#include <cmath>
#include <utility>

namespace pp {

namespace {

constexpr std::array<std::pair<HotkeyMode, const char *>, 6> modeNames{{
    {HotkeyMode::Toggle, "toggle"},
    {HotkeyMode::PushToTalk, "pushToTalk"},
    {HotkeyMode::PushToMute, "pushToMute"},
    {HotkeyMode::Duck, "duck"},
    {HotkeyMode::LevelUp, "levelUp"},
    {HotkeyMode::LevelDown, "levelDown"},
}};

bool suits(HotkeyMode mode, MidiAction action)
{
    switch (mode) {
    case HotkeyMode::Toggle:
        return action != MidiAction::Volume && action != MidiAction::SendLevel;
    case HotkeyMode::PushToTalk:
    case HotkeyMode::PushToMute:
        return action == MidiAction::Mute;
    case HotkeyMode::Duck:
        return action == MidiAction::Volume;
    case HotkeyMode::LevelUp:
    case HotkeyMode::LevelDown:
        return action == MidiAction::Volume || action == MidiAction::SendLevel;
    }
    return false;
}

QVariant roleAt(const DeviceModel &devices, int row, DeviceModel::Role role)
{
    return devices.data(devices.index(row), role);
}

} // namespace

std::string hotkeyModeName(HotkeyMode mode)
{
    for (const auto &[m, name] : modeNames) {
        if (m == mode) {
            return name;
        }
    }
    return {};
}

QJsonObject hotkeyToJson(const Hotkey &hotkey)
{
    return {
        {QStringLiteral("id"), QString::fromStdString(hotkey.id)},
        {QStringLiteral("mode"), QString::fromStdString(hotkeyModeName(hotkey.mode))},
        {QStringLiteral("target"), midiTargetToJson(hotkey.target)},
    };
}

std::optional<Hotkey> hotkeyFromJson(const QJsonObject &json)
{
    const auto target = midiTargetFromJson(json.value(QLatin1String("target")).toObject());
    const auto modeName = json.value(QLatin1String("mode")).toString().toStdString();
    const auto id = json.value(QLatin1String("id")).toString().toStdString();
    for (const auto &[mode, name] : modeNames) {
        if (modeName == name && target && suits(mode, target->action) && !id.empty()) {
            Hotkey hotkey;
            hotkey.id = id;
            hotkey.mode = mode;
            hotkey.target = *target;
            return hotkey;
        }
    }
    return std::nullopt;
}

Hotkeys::Hotkeys(DeviceModel *devices, ShortcutBackend *backend, QObject *parent)
    : QObject(parent)
    , m_devices(devices)
    , m_backend(backend)
    , m_targets(devices)
{
    m_targets.setSceneHandler([this](const QString &name) { Q_EMIT sceneRequested(name); });
    m_targets.setFeedback([this](const QString &icon, const QString &text) { Q_EMIT feedback(icon, text); });
    connect(m_backend, &ShortcutBackend::pressed, this, [this](const QString &id) { press(id, false); });
    connect(m_backend, &ShortcutBackend::repeated, this, [this](const QString &id) { press(id, true); });
    connect(m_backend, &ShortcutBackend::released, this, &Hotkeys::release);
    connect(m_backend, &ShortcutBackend::keysChanged, this, &Hotkeys::changed);
    connect(m_backend, &ShortcutBackend::availableChanged, this, [this] {
        m_registered.clear(); // the service (re)started: register everything again
        sync();
    });
    // Names follow the strips' labels; new and forgotten hotkeys follow the store.
    connect(m_devices, &DeviceModel::hotkeysChanged, this, &Hotkeys::sync);
    connect(m_devices, &QAbstractItemModel::dataChanged, this, &Hotkeys::sync);
    connect(m_devices, &QAbstractItemModel::rowsInserted, this, &Hotkeys::sync);
    connect(m_devices, &QAbstractItemModel::rowsRemoved, this, &Hotkeys::sync);
    sync();
}

void Hotkeys::setSoundboard(Soundboard *soundboard)
{
    m_targets.setPadHandlers([soundboard](const std::string &id) { soundboard->trigger(id); },
                             [soundboard](const std::string &id) { return soundboard->isPlaying(id); });
    // Pads renamed or removed: names and hotkeys follow.
    connect(m_devices, &DeviceModel::soundboardChanged, this, &Hotkeys::sync);
}

std::string Hotkeys::add(HotkeyMode mode, const MidiTarget &target)
{
    if (!suits(mode, target.action)) {
        return {};
    }
    Hotkey hotkey;
    hotkey.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    hotkey.mode = mode;
    hotkey.target = target;
    auto hotkeys = m_devices->store().hotkeys();
    hotkeys.push_back(hotkey);
    m_devices->setHotkeys(std::move(hotkeys));
    return hotkey.id;
}

void Hotkeys::remove(const std::string &id)
{
    auto hotkeys = m_devices->store().hotkeys();
    std::erase_if(hotkeys, [&id](const Hotkey &h) { return h.id == id; });
    m_devices->setHotkeys(std::move(hotkeys));
}

void Hotkeys::setKeys(const std::string &id, const QList<int> &keys)
{
    if (m_registered.contains(id)) {
        m_backend->setKeys(id, keys);
    }
}

std::string Hotkeys::describe(const Hotkey &hotkey) const
{
    const auto strip = [this, &hotkey] {
        const int row = m_devices->rowForKey(hotkey.target.key);
        return row < 0 ? hotkey.target.key : roleAt(*m_devices, row, DeviceModel::LabelRole).toString().toStdString();
    };
    switch (hotkey.mode) {
    case HotkeyMode::Toggle:
        return m_targets.describe(hotkey.target);
    case HotkeyMode::PushToTalk:
        return "Push to talk: " + strip();
    case HotkeyMode::PushToMute:
        return "Push to mute: " + strip();
    case HotkeyMode::Duck:
        return "Duck while held: " + strip();
    case HotkeyMode::LevelUp:
        return m_targets.describe(hotkey.target) + " up";
    case HotkeyMode::LevelDown:
        return m_targets.describe(hotkey.target) + " down";
    }
    return {};
}

void Hotkeys::sync()
{
    if (!m_backend->isAvailable()) {
        return;
    }
    const auto &hotkeys = m_devices->store().hotkeys();
    for (auto it = m_registered.begin(); it != m_registered.end();) {
        if (std::ranges::none_of(hotkeys, [&it](const Hotkey &h) { return h.id == it->first; })) {
            m_backend->remove(it->first);
            m_ducked.erase(it->first);
            it = m_registered.erase(it);
        } else {
            ++it;
        }
    }
    bool renamed = false;
    for (const auto &h : hotkeys) {
        const auto name = "Patch Prime: " + describe(h);
        if (auto at = m_registered.find(h.id); at == m_registered.end() || at->second != name) {
            m_backend->add(h.id, name);
            m_registered[h.id] = name;
            renamed = true;
        }
    }
    if (renamed) {
        Q_EMIT changed();
    }
}

std::optional<Hotkey> Hotkeys::find(const QString &id) const
{
    const auto key = id.toStdString();
    for (const auto &h : m_devices->store().hotkeys()) {
        if (h.id == key) {
            return h;
        }
    }
    return std::nullopt;
}

void Hotkeys::press(const QString &id, bool repeat)
{
    const auto hotkey = find(id);
    if (!hotkey) {
        return;
    }
    const auto &target = hotkey->target;
    const int row = m_devices->rowForKey(target.key);
    const bool stepping = hotkey->mode == HotkeyMode::LevelUp || hotkey->mode == HotkeyMode::LevelDown;
    if (repeat && !stepping) {
        return; // held: only level steps repeat
    }
    switch (hotkey->mode) {
    case HotkeyMode::Toggle:
        m_targets.toggle(target);
        break;
    case HotkeyMode::PushToTalk:
    case HotkeyMode::PushToMute:
        if (row >= 0) {
            m_devices->setMuted(row, hotkey->mode == HotkeyMode::PushToMute);
        }
        break;
    case HotkeyMode::Duck:
        if (row >= 0 && !m_ducked.contains(hotkey->id)) {
            const double before = roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble();
            m_devices->setVolume(row, before - duckDb);
            m_ducked[hotkey->id] = {before, roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble()};
        }
        break;
    case HotkeyMode::LevelUp:
    case HotkeyMode::LevelDown: {
        const double step = hotkey->mode == HotkeyMode::LevelUp ? stepDb : -stepDb;
        if (row < 0) {
            break;
        }
        double now = 0.0;
        if (target.action == MidiAction::Volume) {
            const double db = roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble();
            m_devices->setVolume(row, std::round(db + step));
            now = roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble();
        } else {
            const auto destination = QString::fromStdString(target.destination);
            const double db = roleAt(*m_devices, row, DeviceModel::SendLevelsRole).toMap().value(destination, 0.0).toDouble();
            m_devices->setSendLevel(row, destination, std::round(db + step));
            now = roleAt(*m_devices, row, DeviceModel::SendLevelsRole).toMap().value(destination, 0.0).toDouble();
        }
        Q_EMIT feedback(step > 0 ? QStringLiteral("audio-volume-high") : QStringLiteral("audio-volume-low"),
                        QString::fromStdString(m_targets.describe(target)) + QStringLiteral(" ") +
                            (now <= DeviceModel::minimumVolumeDb() ? QStringLiteral("−∞ dB")
                                                                   : QStringLiteral("%1 dB").arg(now, 0, 'f', 1)));
        break;
    }
    }
}

void Hotkeys::release(const QString &id)
{
    const auto hotkey = find(id);
    if (!hotkey) {
        return;
    }
    const int row = m_devices->rowForKey(hotkey->target.key);
    if (row < 0) {
        return;
    }
    if (hotkey->mode == HotkeyMode::PushToTalk || hotkey->mode == HotkeyMode::PushToMute) {
        m_devices->setMuted(row, hotkey->mode == HotkeyMode::PushToTalk);
    } else if (hotkey->mode == HotkeyMode::Duck) {
        if (const auto at = m_ducked.find(hotkey->id); at != m_ducked.end()) {
            // Back up, unless the level was changed meanwhile.
            const double now = roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble();
            if (std::abs(now - at->second.second) < 0.05) {
                m_devices->setVolume(row, at->second.first);
            }
            m_ducked.erase(at);
        }
    }
}

} // namespace pp

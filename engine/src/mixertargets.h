// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "midimapping.h"

#include <QString>

#include <functional>
#include <optional>
#include <string>

namespace pp {

class DeviceModel;
class RecordingManager;

// What controls outside the app (MIDI controls, hotkeys) act on: a
// MidiTarget, read and changed through the DeviceModel. Levels and
// parameters have a position (0..1, the fader law for levels); the rest
// are on or off.
class MixerTargets
{
public:
    explicit MixerTargets(DeviceModel *devices)
        : m_devices(devices)
    {
    }

    // Optional: recording targets do nothing without it.
    void setRecordings(RecordingManager *recordings) { m_recordings = recordings; }
    // Called when a scene target is toggled (loading scenes isn't the
    // model's job).
    void setSceneHandler(std::function<void(const QString &)> handler) { m_onScene = std::move(handler); }

    // What a target is, for people: "Mic: level", "Mic → Stream Mix: send".
    [[nodiscard]] std::string describe(const MidiTarget &target) const;
    // Levels and parameters (not switches): they have a position.
    [[nodiscard]] bool isContinuous(const MidiTarget &target) const;
    // A target's position, 0..1, or nullopt when it's gone or isn't one.
    [[nodiscard]] std::optional<double> position(const MidiTarget &target) const;
    void setPosition(const MidiTarget &target, double position);
    // Whether an on/off target is on, or nullopt; toggle flips it (a scene
    // is loaded).
    [[nodiscard]] std::optional<bool> isOn(const MidiTarget &target) const;
    void toggle(const MidiTarget &target);

private:
    DeviceModel *m_devices;
    RecordingManager *m_recordings = nullptr;
    std::function<void(const QString &)> m_onScene;
};

} // namespace pp

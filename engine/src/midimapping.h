// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QJsonObject>

#include <cstdint>
#include <optional>
#include <string>

namespace pp {

// What a MIDI control does in the mixer (see MidiMapper).
enum class MidiAction : std::uint8_t
{
    Volume,        // a strip's fader
    Mute,          // on/off
    Cue,           // on/off
    SendLevel,     // a send's level (key -> destination)
    Send,          // a send on/off
    EffectParam,   // an effect's parameter (key, effect index, param)
    EffectEnabled, // an effect on/bypassed
    Record,        // start/stop recording a strip
    Scene,         // load a scene (by name)
    Pad,           // play or stop a soundboard pad (by id)
};
std::string midiActionName(MidiAction action); // "volume", "mute", ...
std::optional<MidiAction> midiActionFromName(const std::string &name);

// The strings' {} are for GCC: without them, designated initializers that
// leave them out warn (-Wmissing-field-initializers).
// NOLINTBEGIN(readability-redundant-member-init)
struct MidiTarget
{
    MidiAction action = MidiAction::Volume;
    std::string key{};         // the strip
    std::string destination{}; // sends: the strip sent to
    int effect = -1;           // effects: the effect's index in the strip's chain
    std::string param{};       // EffectParam: the parameter's name
    std::string scene{};       // Scene: its name
    std::string pad{};         // Pad: its id
    bool operator==(const MidiTarget &) const = default;
};

// The MIDI message a control sends.
enum class MidiKind : std::uint8_t
{
    Cc,        // a controller (faders, knobs; buttons on some controllers)
    Note,      // a button (pads, keys)
    PitchBend, // a 14-bit fader (Mackie-style, per channel)
};
std::string midiKindName(MidiKind kind); // "cc", "note", "pitchbend"

struct MidiMapping
{
    std::string id{}; // stable, for the D-Bus API
    MidiKind kind = MidiKind::Cc;
    int channel = 0; // 0..15
    int number = 0;  // the CC or note number (0 for pitch bend)
    // A relative (endless) encoder: 1..63 turns up, 65..127 down. Otherwise
    // the CC's value is a position.
    bool relative = false;
    MidiTarget target{};
    bool operator==(const MidiMapping &) const = default;

    [[nodiscard]] bool sameControl(const MidiMapping &other) const
    {
        return kind == other.kind && channel == other.channel && number == other.number;
    }
};
// NOLINTEND(readability-redundant-member-init)

QJsonObject midiTargetToJson(const MidiTarget &target);
std::optional<MidiTarget> midiTargetFromJson(const QJsonObject &json);
QJsonObject midiMappingToJson(const MidiMapping &mapping);
std::optional<MidiMapping> midiMappingFromJson(const QJsonObject &json);
// The control for people: "CC 7, channel 1", "Note C3, channel 10", ...
std::string describeMidiControl(const MidiMapping &mapping);

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QJsonObject>

#include <optional>
#include <string>

namespace pp {

// A soundboard pad (see Soundboard): a sound file, played into the
// soundboard's strip.
// The strings' {} are for GCC (designated initializers that leave them out).
// NOLINTBEGIN(readability-redundant-member-init)
struct SoundPad
{
    std::string id{};    // stable, for the D-Bus API and MIDI/hotkey targets
    std::string label{}; // on the pad
    std::string path{};  // the sound file
    double gainDb = 0.0; // its level
    bool loop = false;   // plays until pressed again
    bool operator==(const SoundPad &) const = default;
};
// NOLINTEND(readability-redundant-member-init)

QJsonObject soundPadToJson(const SoundPad &pad);
std::optional<SoundPad> soundPadFromJson(const QJsonObject &json);

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "midimapping.h"

#include <array>
#include <format>
#include <utility>

namespace pp {

namespace {

constexpr std::array<std::pair<MidiAction, const char *>, 10> actionNames{{
    {MidiAction::Volume, "volume"},
    {MidiAction::Mute, "mute"},
    {MidiAction::Cue, "cue"},
    {MidiAction::SendLevel, "sendLevel"},
    {MidiAction::Send, "send"},
    {MidiAction::EffectParam, "effectParam"},
    {MidiAction::EffectEnabled, "effectEnabled"},
    {MidiAction::Record, "record"},
    {MidiAction::Scene, "scene"},
    {MidiAction::Pad, "pad"},
}};

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string str(const QJsonObject &json, const char *name)
{
    return json.value(QLatin1String(name)).toString().toStdString();
}

} // namespace

std::string midiActionName(MidiAction action)
{
    for (const auto &[a, name] : actionNames) {
        if (a == action) {
            return name;
        }
    }
    return {};
}

std::optional<MidiAction> midiActionFromName(const std::string &name)
{
    for (const auto &[a, n] : actionNames) {
        if (name == n) {
            return a;
        }
    }
    return std::nullopt;
}

std::string midiKindName(MidiKind kind)
{
    switch (kind) {
    case MidiKind::Cc:
        return "cc";
    case MidiKind::Note:
        return "note";
    case MidiKind::PitchBend:
        return "pitchbend";
    }
    return {};
}

QJsonObject midiTargetToJson(const MidiTarget &target)
{
    QJsonObject json{{QStringLiteral("action"), qs(midiActionName(target.action))}};
    if (!target.key.empty()) {
        json.insert(QStringLiteral("key"), qs(target.key));
    }
    if (!target.destination.empty()) {
        json.insert(QStringLiteral("destination"), qs(target.destination));
    }
    if (target.effect >= 0) {
        json.insert(QStringLiteral("effect"), target.effect);
    }
    if (!target.param.empty()) {
        json.insert(QStringLiteral("param"), qs(target.param));
    }
    if (!target.scene.empty()) {
        json.insert(QStringLiteral("scene"), qs(target.scene));
    }
    if (!target.pad.empty()) {
        json.insert(QStringLiteral("pad"), qs(target.pad));
    }
    return json;
}

std::optional<MidiTarget> midiTargetFromJson(const QJsonObject &json)
{
    const auto action = midiActionFromName(str(json, "action"));
    if (!action) {
        return std::nullopt;
    }
    MidiTarget target{.action = *action,
                      .key = str(json, "key"),
                      .destination = str(json, "destination"),
                      .effect = json.value(QLatin1String("effect")).toInt(-1),
                      .param = str(json, "param"),
                      .scene = str(json, "scene"),
                      .pad = str(json, "pad")};
    // Each action needs its own fields.
    const bool strip = !target.key.empty();
    switch (target.action) {
    case MidiAction::Volume:
    case MidiAction::Mute:
    case MidiAction::Cue:
    case MidiAction::Record:
        return strip ? std::optional{target} : std::nullopt;
    case MidiAction::SendLevel:
    case MidiAction::Send:
        return strip && !target.destination.empty() ? std::optional{target} : std::nullopt;
    case MidiAction::EffectParam:
        return strip && target.effect >= 0 && !target.param.empty() ? std::optional{target} : std::nullopt;
    case MidiAction::EffectEnabled:
        return strip && target.effect >= 0 ? std::optional{target} : std::nullopt;
    case MidiAction::Scene:
        return target.scene.empty() ? std::nullopt : std::optional{target};
    case MidiAction::Pad:
        return target.pad.empty() ? std::nullopt : std::optional{target};
    }
    return std::nullopt;
}

QJsonObject midiMappingToJson(const MidiMapping &mapping)
{
    return {
        {QStringLiteral("id"), qs(mapping.id)},
        {QStringLiteral("kind"), qs(midiKindName(mapping.kind))},
        {QStringLiteral("channel"), mapping.channel},
        {QStringLiteral("number"), mapping.number},
        {QStringLiteral("relative"), mapping.relative},
        {QStringLiteral("target"), midiTargetToJson(mapping.target)},
    };
}

std::optional<MidiMapping> midiMappingFromJson(const QJsonObject &json)
{
    const auto target = midiTargetFromJson(json.value(QLatin1String("target")).toObject());
    const auto kindName = str(json, "kind");
    std::optional<MidiKind> kind;
    for (const auto k : {MidiKind::Cc, MidiKind::Note, MidiKind::PitchBend}) {
        if (midiKindName(k) == kindName) {
            kind = k;
        }
    }
    const int channel = json.value(QLatin1String("channel")).toInt(-1);
    const int number = json.value(QLatin1String("number")).toInt(-1);
    if (!target || !kind || channel < 0 || channel > 15 || number < 0 || number > 127 ||
        str(json, "id").empty()) {
        return std::nullopt;
    }
    return MidiMapping{.id = str(json, "id"),
                       .kind = *kind,
                       .channel = channel,
                       .number = number,
                       .relative = *kind == MidiKind::Cc && json.value(QLatin1String("relative")).toBool(),
                       .target = *target};
}

std::string describeMidiControl(const MidiMapping &mapping)
{
    const int channel = mapping.channel + 1;
    switch (mapping.kind) {
    case MidiKind::Cc:
        return std::format("CC {}, channel {}", mapping.number, channel);
    case MidiKind::Note: {
        constexpr std::array<const char *, 12> names{"C", "C#", "D", "D#", "E", "F",
                                                     "F#", "G", "G#", "A", "A#", "B"};
        // Middle C (60) is C3, as most controllers name it.
        return std::format("Note {}{}, channel {}", names.at(static_cast<std::size_t>(mapping.number % 12)),
                           (mapping.number / 12) - 2, channel);
    }
    case MidiKind::PitchBend:
        return std::format("Pitch bend, channel {}", channel);
    }
    return {};
}

} // namespace pp

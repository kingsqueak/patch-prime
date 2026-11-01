// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "soundpad.h"

namespace pp {

QJsonObject soundPadToJson(const SoundPad &pad)
{
    return {
        {QStringLiteral("id"), QString::fromStdString(pad.id)},
        {QStringLiteral("label"), QString::fromStdString(pad.label)},
        {QStringLiteral("path"), QString::fromStdString(pad.path)},
        {QStringLiteral("gainDb"), pad.gainDb},
        {QStringLiteral("loop"), pad.loop},
    };
}

std::optional<SoundPad> soundPadFromJson(const QJsonObject &json)
{
    SoundPad pad{.id = json.value(QLatin1String("id")).toString().toStdString(),
                 .label = json.value(QLatin1String("label")).toString().toStdString(),
                 .path = json.value(QLatin1String("path")).toString().toStdString(),
                 .gainDb = json.value(QLatin1String("gainDb")).toDouble(0.0),
                 .loop = json.value(QLatin1String("loop")).toBool()};
    if (pad.id.empty() || pad.path.empty()) {
        return std::nullopt;
    }
    return pad;
}

} // namespace pp

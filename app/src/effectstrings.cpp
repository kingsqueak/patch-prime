// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "effectstrings.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <array>

namespace {

constexpr auto strings = std::to_array<const char *>({
    // Effects
    QT_TRANSLATE_NOOP("Effects", "Gain"),
    QT_TRANSLATE_NOOP("Effects", "Parametric EQ"),
    QT_TRANSLATE_NOOP("Effects", "Multiband compressor"),
    QT_TRANSLATE_NOOP("Effects", "Compressor"),
    QT_TRANSLATE_NOOP("Effects", "De-esser"),
    QT_TRANSLATE_NOOP("Effects", "Noise gate"),
    QT_TRANSLATE_NOOP("Effects", "Expander"),
    QT_TRANSLATE_NOOP("Effects", "Ducker"),
    QT_TRANSLATE_NOOP("Effects", "Noise suppression"),
    QT_TRANSLATE_NOOP("Effects", "Broadcast leveler"),
    QT_TRANSLATE_NOOP("Effects", "Reverb"),
    QT_TRANSLATE_NOOP("Effects", "Delay (line up outputs)"),
    QT_TRANSLATE_NOOP("Effects", "Safety limiter"),
    // Voice chains
    QT_TRANSLATE_NOOP("Effects", "Podcast voice"),
    QT_TRANSLATE_NOOP("Effects", "Streaming voice"),
    QT_TRANSLATE_NOOP("Effects", "Noisy room"),
    QT_TRANSLATE_NOOP("Effects", "Radio voice"),
    QT_TRANSLATE_NOOP("Effects", "Natural voice"),
    // Parameters
    QT_TRANSLATE_NOOP("Effects", "Advanced"),
    QT_TRANSLATE_NOOP("Effects", "Amount"),
    QT_TRANSLATE_NOOP("Effects", "Attack"),
    QT_TRANSLATE_NOOP("Effects", "Ceiling"),
    QT_TRANSLATE_NOOP("Effects", "Damping"),
    QT_TRANSLATE_NOOP("Effects", "Decay"),
    QT_TRANSLATE_NOOP("Effects", "Depth"),
    QT_TRANSLATE_NOOP("Effects", "Freeze below"),
    QT_TRANSLATE_NOOP("Effects", "Frequency"),
    QT_TRANSLATE_NOOP("Effects", "Hold"),
    QT_TRANSLATE_NOOP("Effects", "Listen"),
    QT_TRANSLATE_NOOP("Effects", "Makeup"),
    QT_TRANSLATE_NOOP("Effects", "Max boost"),
    QT_TRANSLATE_NOOP("Effects", "Max cut"),
    QT_TRANSLATE_NOOP("Effects", "Mix (wet)"),
    QT_TRANSLATE_NOOP("Effects", "Pre-delay"),
    QT_TRANSLATE_NOOP("Effects", "Range"),
    QT_TRANSLATE_NOOP("Effects", "Ratio"),
    QT_TRANSLATE_NOOP("Effects", "Release"),
    QT_TRANSLATE_NOOP("Effects", "Size"),
    QT_TRANSLATE_NOOP("Effects", "Speed"),
    QT_TRANSLATE_NOOP("Effects", "Target"),
    QT_TRANSLATE_NOOP("Effects", "Threshold"),
    QT_TRANSLATE_NOOP("Effects", "Time"),
    QT_TRANSLATE_NOOP("Effects", "True peak"),
    QT_TRANSLATE_NOOP("Effects", "Width"),
    // EQ band types, and the multiband's splits
    QT_TRANSLATE_NOOP("Effects", "Bell"),
    QT_TRANSLATE_NOOP("Effects", "Low shelf"),
    QT_TRANSLATE_NOOP("Effects", "High shelf"),
    QT_TRANSLATE_NOOP("Effects", "Low cut"),
    QT_TRANSLATE_NOOP("Effects", "High cut"),
    QT_TRANSLATE_NOOP("Effects", "Crossover 1"),
    QT_TRANSLATE_NOOP("Effects", "Crossover 2"),
    QT_TRANSLATE_NOOP("Effects", "Crossover 3"),
    // After "Band n"
    QT_TRANSLATE_NOOP("Effects", "on"),
    QT_TRANSLATE_NOOP("Effects", "solo"),
    QT_TRANSLATE_NOOP("Effects", "type"),
    QT_TRANSLATE_NOOP("Effects", "frequency"),
    QT_TRANSLATE_NOOP("Effects", "gain"),
    QT_TRANSLATE_NOOP("Effects", "Q"),
    QT_TRANSLATE_NOOP("Effects", "threshold"),
    QT_TRANSLATE_NOOP("Effects", "ratio"),
    QT_TRANSLATE_NOOP("Effects", "attack"),
    QT_TRANSLATE_NOOP("Effects", "release"),
    QT_TRANSLATE_NOOP("Effects", "makeup"),
});

const QRegularExpression &bandText()
{
    static const QRegularExpression pattern(QStringLiteral("^Band (\\d+) (.+)$"));
    return pattern;
}

bool listed(const QString &text)
{
    return std::ranges::any_of(strings, [&text](const char *s) { return text == QLatin1String(s); });
}

} // namespace

QString effectText(const QString &text)
{
    if (const auto band = bandText().match(text); band.hasMatch() && listed(band.captured(2))) {
        return QCoreApplication::translate("Effects", "Band %1 %2")
            .arg(band.captured(1), QCoreApplication::translate("Effects", band.captured(2).toUtf8().constData()));
    }
    return listed(text) ? QCoreApplication::translate("Effects", text.toUtf8().constData()) : text;
}

bool isEffectText(const QString &text)
{
    const auto band = bandText().match(text);
    return listed(band.hasMatch() ? band.captured(2) : text);
}

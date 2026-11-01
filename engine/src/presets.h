// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"

#include <QString>
#include <QStringList>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pp {

// Presets, kept in ~/.config/patchprime/presets.json:
// - chain presets: a strip's whole effects chain under a name ("Podcast
//   mic"), to load onto any strip;
// - effect presets: one effect's settings (its parameters and a plugin's
//   own state) under a name, per effect type ("eq", "lv2:<uri>", ...).
// Names are trimmed; saving under an existing name replaces it.
class PresetStore
{
public:
    // An empty path means the standard file.
    explicit PresetStore(QString path = {});

    [[nodiscard]] const QString &path() const { return m_path; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    bool load();

    [[nodiscard]] QStringList chainNames() const;
    [[nodiscard]] std::optional<std::vector<EffectSettings>> chain(const QString &name) const;
    bool saveChain(const QString &name, const std::vector<EffectSettings> &effects);
    bool removeChain(const QString &name);

    // Effect presets by type: {type: [names]}.
    [[nodiscard]] std::map<std::string, QStringList> effectNames() const;
    [[nodiscard]] std::optional<EffectSettings> effect(const std::string &type, const QString &name) const;
    bool saveEffect(const QString &name, const EffectSettings &effect);
    bool removeEffect(const std::string &type, const QString &name);

    static QString defaultPath();

private:
    bool save();

    QString m_path;
    QString m_lastError;
    std::map<QString, std::vector<EffectSettings>> m_chains;
    std::map<std::string, std::map<QString, EffectSettings>> m_effects; // type -> name -> settings
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "presets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace pp {

PresetStore::PresetStore(QString path)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
}

QString PresetStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/patchprime/presets.json");
}

bool PresetStore::load()
{
    m_chains.clear();
    m_effects.clear();
    m_lastError.clear();
    QFile file(m_path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        m_lastError = parseError.errorString();
        return false;
    }
    const auto root = document.object();
    const auto chains = root.value(QLatin1String("chains")).toObject();
    for (auto it = chains.begin(); it != chains.end(); ++it) {
        m_chains[it.key()] = effectsFromJson(it.value().toArray());
    }
    const auto effects = root.value(QLatin1String("effects")).toObject();
    for (auto type = effects.begin(); type != effects.end(); ++type) {
        const auto named = type.value().toObject();
        for (auto it = named.begin(); it != named.end(); ++it) {
            // One effect, stored as a chain of one (unknown types drop out).
            const auto read = effectsFromJson(QJsonArray{it.value()});
            if (read.size() == 1 && read.front().type == type.key().toStdString()) {
                m_effects[type.key().toStdString()][it.key()] = read.front();
            }
        }
    }
    return true;
}

bool PresetStore::save()
{
    m_lastError.clear();
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_lastError = QStringLiteral("could not create ") + QFileInfo(m_path).absolutePath();
        return false;
    }
    QJsonObject chains;
    for (const auto &[name, effects] : m_chains) {
        chains.insert(name, effectsToJson(effects));
    }
    QJsonObject effects;
    for (const auto &[type, named] : m_effects) {
        QJsonObject byName;
        for (const auto &[name, effect] : named) {
            byName.insert(name, effectsToJson({effect}).at(0));
        }
        effects.insert(QString::fromStdString(type), byName);
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                         {QStringLiteral("chains"), chains},
                                         {QStringLiteral("effects"), effects}})
                   .toJson());
    if (!file.commit()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
}

QStringList PresetStore::chainNames() const
{
    QStringList names;
    for (const auto &[name, effects] : m_chains) {
        names.append(name);
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

std::optional<std::vector<EffectSettings>> PresetStore::chain(const QString &name) const
{
    const auto at = m_chains.find(name.trimmed());
    return at == m_chains.end() ? std::nullopt : std::optional{at->second};
}

bool PresetStore::saveChain(const QString &name, const std::vector<EffectSettings> &effects)
{
    if (name.trimmed().isEmpty()) {
        return false;
    }
    m_chains[name.trimmed()] = effects;
    return save();
}

bool PresetStore::removeChain(const QString &name)
{
    return m_chains.erase(name.trimmed()) > 0 && save();
}

std::map<std::string, QStringList> PresetStore::effectNames() const
{
    std::map<std::string, QStringList> out;
    for (const auto &[type, named] : m_effects) {
        auto &names = out[type];
        for (const auto &[name, effect] : named) {
            names.append(name);
        }
        names.sort(Qt::CaseInsensitive);
    }
    return out;
}

std::optional<EffectSettings> PresetStore::effect(const std::string &type, const QString &name) const
{
    const auto t = m_effects.find(type);
    if (t == m_effects.end()) {
        return std::nullopt;
    }
    const auto at = t->second.find(name.trimmed());
    return at == t->second.end() ? std::nullopt : std::optional{at->second};
}

bool PresetStore::saveEffect(const QString &name, const EffectSettings &effect)
{
    if (name.trimmed().isEmpty() || effect.type.empty()) {
        return false;
    }
    auto stored = effect;
    stored.enabled = true; // a preset is settings, not whether it's bypassed
    stored.key.clear();    // nor which strip a ducker listens to
    m_effects[effect.type][name.trimmed()] = std::move(stored);
    return save();
}

bool PresetStore::removeEffect(const std::string &type, const QString &name)
{
    const auto t = m_effects.find(type);
    if (t == m_effects.end() || t->second.erase(name.trimmed()) == 0) {
        return false;
    }
    if (t->second.empty()) {
        m_effects.erase(t);
    }
    return save();
}

} // namespace pp

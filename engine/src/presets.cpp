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

#include <array>

namespace pp {

PresetStore::PresetStore(QString path)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
}

namespace {

// An EQ band's parameters: b<n>On/Type/Freq/Gain/Q. Types: 0 bell, 1 low
// shelf, 2 high shelf, 3 low cut, 4 high cut (EqEffect::BandType).
void band(std::map<std::string, double> &params, int n, int type, double freq, double gainDb,
          double q)
{
    const auto prefix = "b" + std::to_string(n);
    params[prefix + "On"] = 1.0;
    params[prefix + "Type"] = type;
    params[prefix + "Freq"] = freq;
    params[prefix + "Gain"] = gainDb;
    params[prefix + "Q"] = q;
}

EffectSettings fx(std::string type, std::map<std::string, double> params = {})
{
    return {.type = std::move(type), .enabled = true, .params = std::move(params), .key = {}, .state = {}};
}

std::vector<std::pair<QString, std::vector<EffectSettings>>> voiceChains()
{
    constexpr int bell = 0;
    constexpr int lowShelf = 1;
    constexpr int highShelf = 2;
    constexpr int lowCut = 3;
    const auto eq = [](const std::vector<std::array<double, 5>> &bands) {
        std::map<std::string, double> params;
        for (const auto &b : bands) {
            band(params, static_cast<int>(b[0]), static_cast<int>(b[1]), b[2], b[3], b[4]);
        }
        return fx("eq", std::move(params));
    };
    const auto limiter = fx("limiter", {{"ceiling", -1.0}, {"truePeak", 1.0}});
    std::vector<std::pair<QString, std::vector<EffectSettings>>> chains;
    // Spoken word for a podcast: cleaned up, even, and at -16 LUFS.
    chains.push_back({QStringLiteral("Podcast voice"),
                      {eq({{1, lowCut, 80, 0, 0.707}, {2, bell, 250, -2, 1.0},
                           {4, bell, 3000, 2, 0.8}, {6, highShelf, 12000, 1.5, 0.707}}),
                       fx("expander", {{"threshold", -50.0}, {"ratio", 2.0}, {"range", 15.0}}),
                       fx("compressor", {{"amount", 50.0}}),
                       fx("deesser"),
                       fx("leveler", {{"target", -16.0}}),
                       limiter}});
    // A mic over game or music on a stream: present and steady, room noise out.
    std::vector<EffectSettings> streaming;
#ifdef PATCHPRIME_HAVE_RNNOISE
    streaming.push_back(fx("denoise"));
#else
    streaming.push_back(fx("expander", {{"threshold", -45.0}, {"ratio", 2.0}, {"range", 20.0}}));
#endif
    streaming.push_back(eq({{1, lowCut, 90, 0, 0.707}, {4, bell, 3500, 2.5, 0.8}}));
    streaming.push_back(fx("compressor", {{"amount", 60.0}}));
    streaming.push_back(fx("deesser"));
    streaming.push_back(limiter);
    chains.emplace_back(QStringLiteral("Streaming voice"), std::move(streaming));
    // Fans, traffic, a keyboard: noise suppression and a gate first.
    std::vector<EffectSettings> noisy;
#ifdef PATCHPRIME_HAVE_RNNOISE
    noisy.push_back(fx("denoise"));
#endif
    noisy.push_back(fx("gate", {{"threshold", -45.0}, {"range", 30.0}}));
    noisy.push_back(eq({{1, lowCut, 100, 0, 0.707}, {2, bell, 400, -2, 1.0}}));
    noisy.push_back(fx("compressor", {{"amount", 40.0}}));
    noisy.push_back(fx("deesser"));
    noisy.push_back(limiter);
    chains.emplace_back(QStringLiteral("Noisy room"), std::move(noisy));
    // Close and big, like a radio presenter: more low end, more presence,
    // more compression.
    chains.push_back({QStringLiteral("Radio voice"),
                      {eq({{1, lowCut, 70, 0, 0.707}, {2, lowShelf, 150, 3, 0.707},
                           {4, bell, 3000, 3, 0.7}, {6, highShelf, 10000, 2, 0.707}}),
                       fx("expander", {{"threshold", -48.0}, {"ratio", 2.0}, {"range", 15.0}}),
                       fx("compressor", {{"amount", 75.0}}),
                       fx("deesser", {{"threshold", -32.0}}),
                       fx("leveler", {{"target", -16.0}, {"speed", 3.0}}),
                       limiter}});
    // Just the basics, lightly: rumble out, a little compression, a ceiling.
    chains.push_back({QStringLiteral("Natural voice"),
                      {eq({{1, lowCut, 80, 0, 0.707}}),
                       fx("compressor", {{"amount", 25.0}}),
                       fx("deesser", {{"threshold", -28.0}, {"range", 6.0}}),
                       limiter}});
    return chains;
}

} // namespace

QStringList PresetStore::voiceChainNames()
{
    QStringList names;
    for (const auto &[name, chain] : voiceChains()) {
        names.append(name);
    }
    return names;
}

std::optional<std::vector<EffectSettings>> PresetStore::voiceChain(const QString &name)
{
    for (auto &[n, chain] : voiceChains()) {
        if (n == name) {
            return std::move(chain);
        }
    }
    return std::nullopt;
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

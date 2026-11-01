// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "mixertargets.h"

#include "devicemodel.h"
#include "effects.h"
#include "mackiecontrol.h"
#include "plugins.h"
#include "recordingmanager.h"

#include <QScopeGuard>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {


QVariant roleAt(const DeviceModel &devices, int row, DeviceModel::Role role)
{
    return devices.data(devices.index(row), role);
}

double paramPosition(const ParamInfo &p, double value)
{
    if (p.maximum <= p.minimum) {
        return 0.0;
    }
    if (p.kind == ParamKind::Log && p.minimum > 0.0) {
        return std::log(value / p.minimum) / std::log(p.maximum / p.minimum);
    }
    return (value - p.minimum) / (p.maximum - p.minimum);
}

double paramValue(const ParamInfo &p, double position)
{
    position = std::clamp(position, 0.0, 1.0);
    if (p.kind == ParamKind::Log && p.minimum > 0.0) {
        return validParam(p, p.minimum * std::pow(p.maximum / p.minimum, position));
    }
    return validParam(p, p.minimum + (position * (p.maximum - p.minimum)));
}

double roundedDb(double db)
{
    return std::round(db * 10.0) / 10.0;
}

} // namespace

std::string MixerTargets::describe(const MidiTarget &target) const
{
    const auto label = [this](const std::string &key) {
        const int row = m_devices->rowForKey(key);
        return row < 0 ? key : roleAt(*m_devices, row, DeviceModel::LabelRole).toString().toStdString();
    };
    // The effect's name, and the parameter's label.
    const auto effect = [&]() -> std::pair<std::string, std::string> {
        const int row = m_devices->rowForKey(target.key);
        const auto effects = row < 0 ? QVariantList{} : roleAt(*m_devices, row, DeviceModel::EffectsRole).toList();
        if (target.effect < 0 || target.effect >= effects.size()) {
            return {"effect " + std::to_string(target.effect + 1), target.param};
        }
        const auto type = effects.at(target.effect).toMap().value(QStringLiteral("type")).toString().toStdString();
        std::string name = effectLabel(type);
        if (const auto *catalog = m_devices->pluginCatalog(); catalog != nullptr && isPluginType(type)) {
            if (const auto *info = catalog->find(type)) {
                name = info->name;
            }
        }
        const auto params = paramsFor(type, m_devices->pluginCatalog());
        const auto at = std::ranges::find(params, target.param, &ParamInfo::name);
        return {name, at == params.end() ? target.param : at->label};
    };
    switch (target.action) {
    case MidiAction::Volume:
        return label(target.key) + ": level";
    case MidiAction::Mute:
        return label(target.key) + ": mute";
    case MidiAction::Cue:
        return label(target.key) + ": cue";
    case MidiAction::Record:
        return label(target.key) + ": record";
    case MidiAction::SendLevel:
        return label(target.key) + " → " + label(target.destination) + ": send level";
    case MidiAction::Send:
        return label(target.key) + " → " + label(target.destination) + ": send on/off";
    case MidiAction::EffectParam: {
        const auto [name, param] = effect();
        return label(target.key) + ": " + name + " " + param;
    }
    case MidiAction::EffectEnabled:
        return label(target.key) + ": " + effect().first + " on/off";
    case MidiAction::Scene:
        return "Scene " + target.scene;
    case MidiAction::Pad: {
        const auto &pads = m_devices->store().pads();
        const auto at = std::ranges::find(pads, target.pad, &SoundPad::id);
        return "Pad " + (at == pads.end() ? target.pad : at->label);
    }
    }
    return {};
}

std::optional<double> MixerTargets::position(const MidiTarget &target) const
{
    const int row = m_devices->rowForKey(target.key);
    if (row < 0) {
        return std::nullopt;
    }
    switch (target.action) {
    case MidiAction::Volume:
        return faderLawPosition(roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble());
    case MidiAction::SendLevel: {
        const auto destination = QString::fromStdString(target.destination);
        if (!roleAt(*m_devices, row, DeviceModel::SendsRole).toStringList().contains(destination)) {
            return std::nullopt;
        }
        return faderLawPosition(
            roleAt(*m_devices, row, DeviceModel::SendLevelsRole).toMap().value(destination, 0.0).toDouble());
    }
    case MidiAction::EffectParam: {
        const auto effects = roleAt(*m_devices, row, DeviceModel::EffectsRole).toList();
        if (target.effect >= effects.size()) {
            return std::nullopt;
        }
        const auto effect = effects.at(target.effect).toMap();
        const auto type = effect.value(QStringLiteral("type")).toString().toStdString();
        for (const auto &p : paramsFor(type, m_devices->pluginCatalog())) {
            if (p.name == target.param && p.kind != ParamKind::Toggle) {
                const auto value = effect.value(QStringLiteral("params")).toMap().value(
                    QString::fromStdString(p.name), p.initial);
                return std::clamp(paramPosition(p, value.toDouble()), 0.0, 1.0);
            }
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

std::optional<bool> MixerTargets::isOn(const MidiTarget &target) const
{
    if (target.action == MidiAction::Scene) {
        return m_devices->store().currentScene() == target.scene;
    }
    if (target.action == MidiAction::Pad) {
        return m_padPlaying ? std::optional{m_padPlaying(target.pad)} : std::nullopt;
    }
    const int row = m_devices->rowForKey(target.key);
    if (row < 0) {
        return std::nullopt;
    }
    switch (target.action) {
    case MidiAction::Mute:
        return roleAt(*m_devices, row, DeviceModel::MutedRole).toBool();
    case MidiAction::Cue:
        return roleAt(*m_devices, row, DeviceModel::CuedRole).toBool();
    case MidiAction::Send:
        return roleAt(*m_devices, row, DeviceModel::SendsRole)
            .toStringList()
            .contains(QString::fromStdString(target.destination));
    case MidiAction::Record:
        return m_recordings != nullptr && m_recordings->recording().contains(target.key);
    case MidiAction::EffectEnabled:
    case MidiAction::EffectParam: {
        const auto effects = roleAt(*m_devices, row, DeviceModel::EffectsRole).toList();
        if (target.effect >= effects.size()) {
            return std::nullopt;
        }
        const auto effect = effects.at(target.effect).toMap();
        if (target.action == MidiAction::EffectEnabled) {
            return effect.value(QStringLiteral("enabled")).toBool();
        }
        return effect.value(QStringLiteral("params")).toMap().value(QString::fromStdString(target.param)).toDouble() >
               0.5;
    }
    default:
        return std::nullopt;
    }
}

bool MixerTargets::isContinuous(const MidiTarget &target) const
{
    switch (target.action) {
    case MidiAction::Volume:
    case MidiAction::SendLevel:
        return true;
    case MidiAction::EffectParam: {
        // A switch is a toggle; any other parameter is continuous.
        const int row = m_devices->rowForKey(target.key);
        const auto effects = row < 0 ? QVariantList{} : roleAt(*m_devices, row, DeviceModel::EffectsRole).toList();
        if (target.effect < 0 || target.effect >= effects.size()) {
            return true;
        }
        const auto type = effects.at(target.effect).toMap().value(QStringLiteral("type")).toString().toStdString();
        const auto params = paramsFor(type, m_devices->pluginCatalog());
        const auto at = std::ranges::find(params, target.param, &ParamInfo::name);
        return at == params.end() || at->kind != ParamKind::Toggle;
    }
    default:
        return false;
    }
}

void MixerTargets::setPosition(const MidiTarget &target, double position)
{
    const int row = m_devices->rowForKey(target.key);
    if (row < 0) {
        return;
    }
    position = std::clamp(position, 0.0, 1.0);
    switch (target.action) {
    case MidiAction::Volume:
        m_devices->setVolume(row, roundedDb(faderLawDb(position)));
        break;
    case MidiAction::SendLevel:
        m_devices->setSendLevel(row, QString::fromStdString(target.destination), roundedDb(faderLawDb(position)));
        break;
    case MidiAction::EffectParam: {
        const auto effects = roleAt(*m_devices, row, DeviceModel::EffectsRole).toList();
        if (target.effect >= effects.size()) {
            return;
        }
        const auto type = effects.at(target.effect).toMap().value(QStringLiteral("type")).toString().toStdString();
        for (const auto &p : paramsFor(type, m_devices->pluginCatalog())) {
            if (p.name == target.param) {
                m_devices->setEffectParam(row, target.effect, QString::fromStdString(p.name), paramValue(p, position));
            }
        }
        break;
    }
    default:
        break;
    }
}

void MixerTargets::toggle(const MidiTarget &target)
{
    if (target.action == MidiAction::Scene) {
        if (m_onScene) {
            m_onScene(QString::fromStdString(target.scene));
        }
        announce(target);
        return;
    }
    if (target.action == MidiAction::Pad) {
        if (m_triggerPad) {
            m_triggerPad(target.pad); // heard: no message
        }
        return;
    }
    const int row = m_devices->rowForKey(target.key);
    const auto on = isOn(target);
    if (row < 0 || !on) {
        return;
    }
    auto announceAfter = qScopeGuard([this, &target] { announce(target); });
    switch (target.action) {
    case MidiAction::Mute:
        m_devices->setMuted(row, !*on);
        break;
    case MidiAction::Cue:
        m_devices->setCue(row, !*on);
        break;
    case MidiAction::Send:
        m_devices->setSend(row, QString::fromStdString(target.destination), !*on);
        break;
    case MidiAction::EffectEnabled:
        m_devices->setEffectEnabled(row, target.effect, !*on);
        break;
    case MidiAction::EffectParam:
        m_devices->setEffectParam(row, target.effect, QString::fromStdString(target.param), *on ? 0.0 : 1.0);
        break;
    case MidiAction::Record:
        if (m_recordings != nullptr) {
            if (*on) {
                m_recordings->stop(target.key);
            } else {
                m_recordings->start(target.key);
            }
        }
        break;
    default:
        break;
    }
}

void MixerTargets::announce(const MidiTarget &target) const
{
    if (!m_feedback) {
        return;
    }
    if (target.action == MidiAction::Scene) {
        m_feedback(QStringLiteral("view-media-playlist"), QStringLiteral("Scene: ") + QString::fromStdString(target.scene));
        return;
    }
    const int row = m_devices->rowForKey(target.key);
    const auto on = isOn(target);
    if (row < 0 || !on) {
        return;
    }
    const auto label = roleAt(*m_devices, row, DeviceModel::LabelRole).toString();
    const bool input = roleAt(*m_devices, row, DeviceModel::StageRole).toString() == QLatin1String("input");
    switch (target.action) {
    case MidiAction::Mute: {
        QString icon = *on ? QStringLiteral("audio-volume-muted") : QStringLiteral("audio-volume-high");
        if (input) {
            icon = *on ? QStringLiteral("microphone-sensitivity-muted") : QStringLiteral("audio-input-microphone");
        }
        m_feedback(icon, *on ? label + QStringLiteral(" muted") : label + QStringLiteral(" on"));
        break;
    }
    case MidiAction::Cue:
        m_feedback(QStringLiteral("audio-headphones"), *on ? label + QStringLiteral(" cued") : label + QStringLiteral(": cue off"));
        break;
    case MidiAction::Record:
        m_feedback(QStringLiteral("media-record"),
                   *on ? QStringLiteral("Recording ") + label : label + QStringLiteral(": recording stopped"));
        break;
    default:
        // Sends and effects: "Mic → Stream Mix: send on/off", "Mic: Gate on/off".
        m_feedback(QStringLiteral("view-media-equalizer"),
                   QString::fromStdString(describe(target)).remove(QStringLiteral(" on/off")) +
                       (*on ? QStringLiteral(" on") : QStringLiteral(" off")));
        break;
    }
}

} // namespace pp

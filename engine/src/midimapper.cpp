// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "midimapper.h"

#include "devicemodel.h"
#include "effects.h"
#include "mackiecontrol.h"
#include "plugins.h"
#include "recordingmanager.h"

#include <QUuid>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

constexpr int ccTop = 127;
constexpr int pitchBendTop = 16383;
constexpr double dbPerClick = 0.5;    // a relative encoder on a level
constexpr double stepPerClick = 0.01; // ... on a parameter (1 % of its range)

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

MidiMapper::MidiMapper(DeviceModel *devices, MidiPort *port, QObject *parent)
    : QObject(parent)
    , m_devices(devices)
    , m_port(port)
{
    m_feedbackTimer.setSingleShot(true);
    m_feedbackTimer.setInterval(20);
    connect(&m_feedbackTimer, &QTimer::timeout, this, &MidiMapper::sendFeedback);

    connect(m_port, &MidiPort::received, this, [this](const QByteArray &bytes) {
        m_parser.feed(bytes, [this](const std::vector<std::uint8_t> &message) { handle(message); });
    });
    connect(m_port, &MidiPort::connectedChanged, this, [this] {
        m_parser.reset();
        m_live.clear(); // a controller (re)connected: send it everything
        scheduleFeedback();
    });
    connect(m_devices, &QAbstractItemModel::dataChanged, this, &MidiMapper::scheduleFeedback);
    connect(m_devices, &QAbstractItemModel::rowsInserted, this, &MidiMapper::scheduleFeedback);
    connect(m_devices, &QAbstractItemModel::rowsRemoved, this, &MidiMapper::scheduleFeedback);
    connect(m_devices, &QAbstractItemModel::modelReset, this, &MidiMapper::scheduleFeedback);
    connect(m_devices, &DeviceModel::cueChanged, this, &MidiMapper::scheduleFeedback);
    connect(m_devices, &DeviceModel::midiMappingsChanged, this, &MidiMapper::scheduleFeedback);
}

void MidiMapper::setRecordings(RecordingManager *recordings)
{
    m_recordings = recordings;
    connect(m_recordings, &RecordingManager::changed, this, &MidiMapper::scheduleFeedback);
}

void MidiMapper::startLearn(const MidiTarget &target)
{
    m_learning = target;
    Q_EMIT learningChanged();
}

void MidiMapper::cancelLearn()
{
    if (m_learning) {
        m_learning.reset();
        Q_EMIT learningChanged();
    }
}

void MidiMapper::remove(const std::string &id)
{
    auto mappings = m_devices->store().midiMappings();
    std::erase_if(mappings, [&id](const MidiMapping &m) { return m.id == id; });
    m_live.erase(id);
    m_devices->setMidiMappings(std::move(mappings));
}

void MidiMapper::setRelative(const std::string &id, bool relative)
{
    auto mappings = m_devices->store().midiMappings();
    for (auto &m : mappings) {
        if (m.id == id && m.kind == MidiKind::Cc) {
            m.relative = relative;
            m_live.erase(id);
        }
    }
    m_devices->setMidiMappings(std::move(mappings));
}

std::string MidiMapper::describe(const MidiTarget &target) const
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
    }
    return {};
}

std::optional<double> MidiMapper::position(const MidiTarget &target) const
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

std::optional<bool> MidiMapper::isOn(const MidiTarget &target) const
{
    if (target.action == MidiAction::Scene) {
        return m_devices->store().currentScene() == target.scene;
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

bool MidiMapper::isContinuous(const MidiTarget &target) const
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

void MidiMapper::handle(const std::vector<std::uint8_t> &message)
{
    if (message.size() != 3) {
        return;
    }
    const int status = message[0] & 0xF0;
    const int channel = message[0] & 0x0F;
    MidiKind kind{};
    int number = message[1];
    int value = message[2];
    bool pressed = false;
    if (status == 0x90 || status == 0x80) {
        kind = MidiKind::Note;
        pressed = status == 0x90 && value > 0;
        value = pressed ? value : 0;
    } else if (status == 0xB0) {
        kind = MidiKind::Cc;
    } else if (status == 0xE0) {
        kind = MidiKind::PitchBend;
        value = message[1] | (message[2] << 7);
        number = 0;
    } else {
        return;
    }
    if (m_learning) {
        if (kind != MidiKind::Note || pressed) {
            learn(kind, channel, number, value);
        }
        return;
    }
    const auto mappings = m_devices->store().midiMappings(); // a copy: applying may change them
    for (const auto &m : mappings) {
        if (m.kind == kind && m.channel == channel && m.number == number) {
            apply(m, value, pressed);
        }
    }
}

void MidiMapper::learn(MidiKind kind, int channel, int number, int value)
{
    if (!m_learning) {
        return;
    }
    MidiMapping mapping{.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                        .kind = kind,
                        .channel = channel,
                        .number = number,
                        .target = *m_learning};
    auto mappings = m_devices->store().midiMappings();
    std::erase_if(mappings, [&mapping](const MidiMapping &m) {
        return m.sameControl(mapping) || m.target == mapping.target;
    });
    mappings.push_back(mapping);
    m_live[mapping.id] = Live{.lastIn = value};
    m_learning.reset();
    m_devices->setMidiMappings(std::move(mappings));
    Q_EMIT learningChanged();
}

void MidiMapper::apply(const MidiMapping &mapping, int value, bool pressed)
{
    auto &live = m_live[mapping.id];
    const auto &target = mapping.target;
    if (!isContinuous(target)) {
        // A button: a note pressed, or a CC going over half way.
        const bool trigger = mapping.kind == MidiKind::Note
                                 ? pressed
                                 : mapping.kind == MidiKind::Cc && value >= 64 && live.lastIn < 64;
        live.lastIn = value;
        if (trigger) {
            toggle(target);
        }
        return;
    }
    if (mapping.kind == MidiKind::Note) {
        return; // a button can't set a level
    }
    const auto current = position(target);
    if (!current) {
        live.lastIn = value;
        return;
    }
    if (mapping.relative) {
        const int clicks = (value & 0x40) != 0 ? -(value & 0x3F) : (value & 0x3F);
        if (target.action == MidiAction::Volume || target.action == MidiAction::SendLevel) {
            setPosition(target, faderLawPosition(faderLawDb(*current) + (clicks * dbPerClick)));
        } else {
            setPosition(target, *current + (clicks * stepPerClick));
        }
        return;
    }
    const double top = mapping.kind == MidiKind::PitchBend ? pitchBendTop : ccTop;
    const double wanted = value / top;
    constexpr double near = 1.5 / ccTop;
    // Pickup: moved elsewhere (the app, a scene) since, the control has to
    // reach the value again before it takes over.
    if (live.pickedUp && live.lastSet >= 0.0 && std::abs(*current - live.lastSet) > near) {
        live.pickedUp = false;
    }
    if (!live.pickedUp) {
        // Reached: the move from its last value passed (or nearly) the
        // current one.
        const double from = live.lastIn >= 0 ? live.lastIn / top : wanted;
        live.pickedUp = *current >= std::min(from, wanted) - near && *current <= std::max(from, wanted) + near;
    }
    live.lastIn = value;
    if (live.pickedUp) {
        setPosition(target, wanted);
        live.lastSet = position(target).value_or(wanted);
    }
}

void MidiMapper::setPosition(const MidiTarget &target, double position)
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

void MidiMapper::toggle(const MidiTarget &target)
{
    if (target.action == MidiAction::Scene) {
        Q_EMIT sceneRequested(QString::fromStdString(target.scene));
        return;
    }
    const int row = m_devices->rowForKey(target.key);
    const auto on = isOn(target);
    if (row < 0 || !on) {
        return;
    }
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

void MidiMapper::scheduleFeedback()
{
    if (m_port->isConnected() && !m_feedbackTimer.isActive()) {
        m_feedbackTimer.start();
    }
}

void MidiMapper::sendFeedback()
{
    if (!m_port->isConnected()) {
        return;
    }
    for (const auto &m : m_devices->store().midiMappings()) {
        if (m.relative) {
            continue; // an endless encoder has no position to show
        }
        const bool continuous = isContinuous(m.target);
        int value = -1;
        if (continuous && m.kind != MidiKind::Note) {
            if (const auto p = position(m.target)) {
                value = static_cast<int>(std::lround(*p * (m.kind == MidiKind::PitchBend ? pitchBendTop : ccTop)));
            }
        } else if (!continuous && m.kind != MidiKind::PitchBend) {
            if (const auto on = isOn(m.target)) {
                value = *on ? ccTop : 0; // the button's light
            }
        }
        auto &live = m_live[m.id];
        if (value < 0 || value == live.lastSent) {
            continue;
        }
        live.lastSent = value;
        if (continuous && value == live.lastIn) {
            continue; // where the control already is
        }
        const auto status = [&m](int kind) { return static_cast<std::uint8_t>(kind | m.channel); };
        const auto number = static_cast<std::uint8_t>(m.number);
        switch (m.kind) {
        case MidiKind::Cc:
            m_port->send(std::vector<std::uint8_t>{status(0xB0), number, static_cast<std::uint8_t>(value)});
            break;
        case MidiKind::Note:
            m_port->send(std::vector<std::uint8_t>{status(0x90), number, static_cast<std::uint8_t>(value)});
            break;
        case MidiKind::PitchBend:
            m_port->send(std::vector<std::uint8_t>{status(0xE0), static_cast<std::uint8_t>(value & 0x7F),
                                                   static_cast<std::uint8_t>(value >> 7)});
            break;
        }
    }
}

} // namespace pp

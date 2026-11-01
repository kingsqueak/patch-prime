// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "midimapper.h"

#include "devicemodel.h"
#include "mackiecontrol.h"
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

} // namespace

MidiMapper::MidiMapper(DeviceModel *devices, MidiPort *port, QObject *parent)
    : QObject(parent)
    , m_devices(devices)
    , m_port(port)
    , m_targets(devices)
{
    m_targets.setSceneHandler([this](const QString &name) { Q_EMIT sceneRequested(name); });
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
    m_targets.setRecordings(recordings);
    connect(recordings, &RecordingManager::changed, this, &MidiMapper::scheduleFeedback);
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
    if (!m_targets.isContinuous(target)) {
        // A button: a note pressed, or a CC going over half way.
        const bool trigger = mapping.kind == MidiKind::Note
                                 ? pressed
                                 : mapping.kind == MidiKind::Cc && value >= 64 && live.lastIn < 64;
        live.lastIn = value;
        if (trigger) {
            m_targets.toggle(target);
        }
        return;
    }
    if (mapping.kind == MidiKind::Note) {
        return; // a button can't set a level
    }
    const auto current = m_targets.position(target);
    if (!current) {
        live.lastIn = value;
        return;
    }
    if (mapping.relative) {
        const int clicks = (value & 0x40) != 0 ? -(value & 0x3F) : (value & 0x3F);
        if (target.action == MidiAction::Volume || target.action == MidiAction::SendLevel) {
            m_targets.setPosition(target, faderLawPosition(faderLawDb(*current) + (clicks * dbPerClick)));
        } else {
            m_targets.setPosition(target, *current + (clicks * stepPerClick));
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
        m_targets.setPosition(target, wanted);
        live.lastSet = m_targets.position(target).value_or(wanted);
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
        const bool continuous = m_targets.isContinuous(m.target);
        int value = -1;
        if (continuous && m.kind != MidiKind::Note) {
            if (const auto p = m_targets.position(m.target)) {
                value = static_cast<int>(std::lround(*p * (m.kind == MidiKind::PitchBend ? pitchBendTop : ccTop)));
            }
        } else if (!continuous && m.kind != MidiKind::PitchBend) {
            if (const auto on = m_targets.isOn(m.target)) {
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

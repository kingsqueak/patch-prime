// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "soundboard.h"

#include "busmanager.h"
#include "devicemodel.h"
#include "pipewiregraph.h"
#include "recording.h"

#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

float linearGain(double db)
{
    return db <= DeviceModel::minimumVolumeDb()
               ? 0.0F
               : static_cast<float>(std::pow(10.0, std::min(db, DeviceModel::maximumVolumeDb()) / 20.0));
}

} // namespace

Soundboard::Soundboard(PipeWireGraph *graph, DeviceModel *devices, BusManager *buses, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_devices(devices)
    , m_buses(buses)
{
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, &Soundboard::poll);
    // Streams belong to the connection: a lost one ends every pad.
    connect(m_graph, &PipeWireGraph::connectedChanged, this, [this](bool connected) {
        if (!connected && !m_playing.empty()) {
            m_playing.clear();
            updateStreams();
            Q_EMIT changed();
        }
    });
    connect(m_devices, &DeviceModel::soundboardChanged, this, [this] {
        updateStreams(); // the target may have changed
        Q_EMIT changed();
    });
}

Soundboard::~Soundboard()
{
    for (const auto &[id, player] : m_playing) {
        m_graph->removeStream(nodeName(id));
    }
}

std::string Soundboard::nodeName(const std::string &id)
{
    // Short: node names show up in tools like qpwgraph.
    return std::string(streamPrefix) + id.substr(0, 8);
}

const SoundPad *Soundboard::find(const std::string &id) const
{
    const auto &pads = m_devices->store().pads();
    const auto at = std::ranges::find(pads, id, &SoundPad::id);
    return at == pads.end() ? nullptr : &*at;
}

std::string Soundboard::add(const QString &path, QString *error)
{
    QString message;
    if (!Player::open(path, &message)) { // readable, and a sound
        if (error != nullptr) {
            *error = message;
        }
        return {};
    }
    SoundPad pad{.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                 .label = QFileInfo(path).completeBaseName().toStdString(),
                 .path = path.toStdString()};
    auto pads = m_devices->store().pads();
    pads.push_back(pad);
    m_devices->setPads(std::move(pads));
    // Somewhere to play from the start: the first mix bus, else a routed
    // input (Desktop).
    if (m_devices->store().soundboardTarget().empty()) {
        std::string fallback;
        for (int row = 0; row < m_devices->rowCount(); ++row) {
            const auto index = m_devices->index(row);
            if (!m_devices->data(index, DeviceModel::EnabledRole).toBool()) {
                continue;
            }
            const auto kind = m_devices->data(index, DeviceModel::KindRole).toString();
            const auto key = m_devices->data(index, DeviceModel::KeyRole).toString().toStdString();
            if (kind == QLatin1String("virtual-sink")) {
                fallback = key;
                break;
            }
            if (kind == QLatin1String("virtual-input") && fallback.empty()) {
                fallback = key;
            }
        }
        if (!fallback.empty()) {
            m_devices->setSoundboardTarget(fallback);
        }
    }
    return pad.id;
}

void Soundboard::remove(const std::string &id)
{
    stop(id);
    auto pads = m_devices->store().pads();
    std::erase_if(pads, [&id](const SoundPad &p) { return p.id == id; });
    m_devices->setPads(std::move(pads));
    // Controls for it go with it.
    auto mappings = m_devices->store().midiMappings();
    std::erase_if(mappings, [&id](const MidiMapping &m) { return m.target.pad == id; });
    m_devices->setMidiMappings(std::move(mappings));
    auto hotkeys = m_devices->store().hotkeys();
    std::erase_if(hotkeys, [&id](const Hotkey &h) { return h.target.pad == id; });
    m_devices->setHotkeys(std::move(hotkeys));
}

bool Soundboard::update(const std::string &id, const std::string &label, double gainDb, bool loop)
{
    auto pads = m_devices->store().pads();
    const auto at = std::ranges::find(pads, id, &SoundPad::id);
    if (at == pads.end()) {
        return false;
    }
    at->label = label.empty() ? at->label : label;
    at->gainDb = std::clamp(gainDb, DeviceModel::minimumVolumeDb(), DeviceModel::maximumVolumeDb());
    at->loop = loop;
    if (const auto playing = m_playing.find(id); playing != m_playing.end()) {
        playing->second->setGain(linearGain(at->gainDb));
        playing->second->setLoop(loop);
    }
    m_devices->setPads(std::move(pads));
    return true;
}

void Soundboard::move(int from, int to)
{
    auto pads = m_devices->store().pads();
    const auto size = static_cast<int>(pads.size());
    if (from < 0 || from >= size || to < 0 || to >= size || from == to) {
        return;
    }
    auto pad = std::move(pads[static_cast<std::size_t>(from)]);
    pads.erase(pads.begin() + from);
    pads.insert(pads.begin() + to, std::move(pad));
    m_devices->setPads(std::move(pads));
}

bool Soundboard::trigger(const std::string &id, QString *error)
{
    if (isPlaying(id)) {
        stop(id);
        return true;
    }
    const auto *pad = find(id);
    if (pad == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("no such pad");
        }
        return false;
    }
    QString message;
    const bool ok = play(*pad, &message);
    if (!ok) {
        m_lastError = message;
        if (error != nullptr) {
            *error = message;
        }
        Q_EMIT changed();
    }
    return ok;
}

bool Soundboard::play(const SoundPad &pad, QString *error)
{
    const auto &target = m_devices->store().soundboardTarget();
    if (target.empty() || m_devices->rowForKey(target) < 0) {
        *error = QStringLiteral("Choose where the soundboard plays first");
        return false;
    }
    if (m_playing.size() >= maxPlaying) {
        *error = QStringLiteral("Too many pads playing at once");
        return false;
    }
    auto player = Player::open(QString::fromStdString(pad.path), error);
    if (!player) {
        return false;
    }
    player->setGain(linearGain(pad.gainDb));
    player->setLoop(pad.loop);
    const Properties props{
        {"node.description", "Patch Prime pad: " + pad.label},
        {"patchprime.tap.owner", m_buses->owner()},
        {"media.role", "Production"},
        {"media.name", pad.label},
    };
    if (!m_graph->addStream(nodeName(pad.id), false, player->fileRate(), props, player)) {
        *error = QStringLiteral("PipeWire refused the pad's stream");
        return false;
    }
    m_playing[pad.id] = std::move(player);
    m_lastError.clear();
    updateStreams();
    m_poll.start();
    Q_EMIT changed();
    return true;
}

void Soundboard::stop(const std::string &id)
{
    if (m_playing.erase(id) > 0) {
        m_graph->removeStream(nodeName(id));
        updateStreams();
        Q_EMIT changed();
    }
}

void Soundboard::stopAll()
{
    if (m_playing.empty()) {
        return;
    }
    for (const auto &[id, player] : m_playing) {
        m_graph->removeStream(nodeName(id));
    }
    m_playing.clear();
    updateStreams();
    Q_EMIT changed();
}

std::set<std::string> Soundboard::playing() const
{
    std::set<std::string> out;
    for (const auto &[id, player] : m_playing) {
        out.insert(id);
    }
    return out;
}

std::map<std::string, std::pair<double, double>> Soundboard::progress() const
{
    std::map<std::string, std::pair<double, double>> out;
    for (const auto &[id, player] : m_playing) {
        out[id] = {player->position(), player->duration()};
    }
    return out;
}

void Soundboard::poll()
{
    std::vector<std::string> done;
    for (const auto &[id, player] : m_playing) {
        if (player->finished()) {
            done.push_back(id);
        }
    }
    for (const auto &id : done) {
        stop(id);
    }
    if (m_playing.empty()) {
        m_poll.stop();
    }
}

void Soundboard::updateStreams()
{
    std::map<std::string, std::string> streams;
    const auto &target = m_devices->store().soundboardTarget();
    for (const auto &[id, player] : m_playing) {
        streams[nodeName(id)] = target;
    }
    m_buses->setPadStreams(std::move(streams));
}

} // namespace pp

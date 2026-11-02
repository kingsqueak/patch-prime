// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "loudnessmanager.h"

#include "busmanager.h"
#include "devicemodel.h"
#include "pipewiregraph.h"
#include "recordingmanager.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcLoudness, "patchprime.loudness")

namespace pp {

LoudnessManager::LoudnessManager(PipeWireGraph *graph, DeviceModel *devices, BusManager *buses,
                                 RecordingManager *recordings, QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices), m_buses(buses), m_recordings(recordings)
{
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, &LoudnessManager::poll);
    // Strips come and go in bursts: look once they've settled.
    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(50);
    connect(&m_updateTimer, &QTimer::timeout, this, &LoudnessManager::update);
    const auto later = [this] { m_updateTimer.start(); };
    connect(graph, &PipeWireGraph::connectedChanged, this, later);
    connect(devices, &DeviceModel::settingsChanged, this, later);
    connect(devices, &QAbstractItemModel::modelReset, this, later);
    connect(devices, &QAbstractItemModel::rowsInserted, this, later);
    connect(devices, &QAbstractItemModel::rowsRemoved, this, later);
    connect(devices, &QAbstractItemModel::dataChanged, this, later);
    connect(devices, &DeviceModel::loudnessSettingsChanged, this, [this] {
        m_updateTimer.start();
        Q_EMIT changed();
    });
    if (recordings != nullptr) {
        connect(recordings, &RecordingManager::changed, this, &LoudnessManager::onRecordingsChanged);
    }
    m_updateTimer.start();
}

LoudnessManager::~LoudnessManager()
{
    stopMeter();
}

std::string LoudnessManager::strip() const
{
    return m_devices->store().loudnessStrip();
}

void LoudnessManager::setStrip(const std::string &key)
{
    m_devices->setLoudnessStrip(key);
}

double LoudnessManager::target() const
{
    return m_devices->store().loudnessTarget();
}

double LoudnessManager::peakTarget() const
{
    return m_devices->store().loudnessPeakTarget();
}

void LoudnessManager::setTargets(double lufs, double dbtp)
{
    m_devices->setLoudnessTargets(lufs, dbtp);
}

void LoudnessManager::reset()
{
    if (m_meter) {
        m_meter->take(); // what was queued belongs to before
    }
    m_analysis.reset();
}

void LoudnessManager::update()
{
    const auto key = strip();
    const int row = key.empty() ? -1 : m_devices->rowForKey(key);
    const bool running = row >= 0 && m_graph->isConnected() &&
                         !m_devices->data(m_devices->index(row), DeviceModel::BusNameRole).toString().isEmpty();
    auto positions = running ? m_buses->stripPositions(key) : std::vector<std::string>{};
    if (running && positions.empty()) {
        positions = {"FL", "FR"};
    }
    if (positions.size() > 2) { // measured as the strip mixes: mono or stereo
        positions.resize(2);
    }
    if (m_meter && key == m_measured && positions == m_positions) {
        return;
    }
    const bool wasMeasuring = m_meter != nullptr;
    if (key != m_measured) {
        m_analysis.reset(); // another strip: start over
    }
    stopMeter();
    if (!running) {
        if (wasMeasuring) {
            Q_EMIT changed();
        }
        return;
    }
    auto meter = std::make_shared<LoudnessMeter>();
    const Properties props{
        {"node.description", "Patch Prime loudness meter"},
        {"patchprime.tap.owner", m_buses->owner()},
        {"media.class", "Audio/Patchprime/Recorder"}, // left alone like the recorders
    };
    if (!m_graph->addStream(std::string(nodeName), true, 0, props, meter, positions)) {
        qCWarning(lcLoudness, "PipeWire refused the loudness meter");
        return;
    }
    m_meter = std::move(meter);
    m_measured = key;
    m_positions = positions;
    m_buses->setLoudnessTap(std::string(nodeName),
                            BusManager::TapTrack{.key = key, .channels = positions.size(), .raw = false});
    m_poll.start();
    qCInfo(lcLoudness, "measuring %s", key.c_str());
    Q_EMIT changed();
}

void LoudnessManager::stopMeter()
{
    if (!m_meter) {
        return;
    }
    m_poll.stop();
    m_buses->setLoudnessTap({}, {});
    m_graph->removeStream(std::string(nodeName));
    m_meter.reset();
}

void LoudnessManager::poll()
{
    if (m_meter) {
        for (const auto &block : m_meter->take()) {
            m_analysis.add(block);
        }
    }
}

void LoudnessManager::onRecordingsChanged()
{
    std::set<std::string> now;
    for (const auto &[key, path] : m_recordings->recording()) {
        now.insert(key);
    }
    if (const auto key = strip(); !key.empty() && now.contains(key) && !m_wasRecording.contains(key)) {
        qCInfo(lcLoudness, "%s started recording: measuring from here", key.c_str());
        reset();
    }
    m_wasRecording = std::move(now);
}

} // namespace pp

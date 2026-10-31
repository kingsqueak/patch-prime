// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "recordingmanager.h"

#include "busmanager.h"
#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <sndfile.h>

#include <algorithm>

Q_LOGGING_CATEGORY(lcRecorder, "patchprime.recorder")

namespace pp {

bool canRecord(EndpointKind kind)
{
    return kind == EndpointKind::VirtualSink || kind == EndpointKind::VirtualInput;
}

bool canPlayInto(EndpointKind kind, Direction direction)
{
    return kind == EndpointKind::VirtualSink || kind == EndpointKind::VirtualInput ||
           (kind == EndpointKind::Hardware && direction == Direction::Output);
}

RecordingManager::RecordingManager(PipeWireGraph *graph, DeviceModel *devices, BusManager *buses,
                                   QString folder, QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices), m_buses(buses),
      m_folder(folder.isEmpty() ? defaultFolder() : std::move(folder))
{
    m_poll.setInterval(200);
    connect(&m_poll, &QTimer::timeout, this, &RecordingManager::poll);
    // A lost connection takes the streams with it: close the files.
    connect(graph, &PipeWireGraph::connectedChanged, this, [this](bool connected) {
        if (!connected) {
            while (!m_recordings.empty()) {
                finish(m_recordings.begin());
            }
            stopPlayback();
        }
    });
    connect(devices, &DeviceModel::recordingSettingsChanged, this, &RecordingManager::changed);
}

RecordingManager::~RecordingManager()
{
    while (!m_recordings.empty()) {
        finish(m_recordings.begin());
    }
    stopPlayback();
}

QString RecordingManager::defaultFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::MusicLocation) +
           QStringLiteral("/Patch Prime");
}

RecordingFormat RecordingManager::format() const
{
    return recordingFormatFromName(m_devices->store().recordingFormat());
}

void RecordingManager::setFormat(RecordingFormat format)
{
    m_devices->setRecordingFormat(recordingFormatName(format));
}

bool RecordingManager::start(const std::string &key, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (m_recordings.contains(key)) {
        return true;
    }
    const int row = m_devices->rowForKey(key);
    if (row < 0) {
        return fail(QStringLiteral("no such strip"));
    }
    const auto index = m_devices->index(row);
    const auto kind =
        kindFromName(m_devices->data(index, DeviceModel::KindRole).toString().toStdString());
    const auto busName = m_devices->data(index, DeviceModel::BusNameRole).toString().toStdString();
    if (!canRecord(kind)) {
        return fail(QStringLiteral("only mix buses and routed inputs can be recorded"));
    }
    if (busName.empty() || !m_graph->isConnected()) {
        return fail(QStringLiteral("the strip isn't running"));
    }
    const auto label = m_devices->data(index, DeviceModel::LabelRole).toString();
    const auto path = uniquePath(
        QDir(m_folder).filePath(recordingFileName(label, QDateTime::currentDateTime(), format())));
    auto recorder = std::make_shared<Recorder>(path, format());
    const auto node = std::string(recorderPrefix) +
                      (busName.starts_with("patchprime.") ? busName.substr(11) : busName);
    const Properties props{
        {"node.description", "Patch Prime recorder (" + label.toStdString() + ")"},
        {"patchprime.tap.owner", m_buses->owner()},
        // Its own class, so Plasma and WirePlumber leave it alone (like sends).
        {"media.class", "Audio/Patchprime/Recorder"},
    };
    if (!m_graph->addStream(node, true, 0, props, recorder)) {
        recorder->stop();
        return fail(QStringLiteral("PipeWire refused the recording stream"));
    }
    qCInfo(lcRecorder, "recording %s to %s", key.c_str(), qPrintable(path));
    m_recordings[key] = Recording{.node = node, .recorder = std::move(recorder)};
    m_lastError.clear();
    updateTaps();
    m_poll.start();
    Q_EMIT changed();
    return true;
}

void RecordingManager::stop(const std::string &key)
{
    if (const auto it = m_recordings.find(key); it != m_recordings.end()) {
        finish(it);
        updateTaps();
        Q_EMIT changed();
    }
}

void RecordingManager::finish(std::map<std::string, Recording>::iterator it)
{
    m_graph->removeStream(it->second.node);
    auto &recorder = *it->second.recorder;
    recorder.stop();
    if (recorder.failed()) {
        m_lastError = QFileInfo(recorder.path()).fileName() + QStringLiteral(": ") + recorder.error();
    } else if (recorder.droppedFrames() > 0) {
        qCWarning(lcRecorder, "%s lost %llu frames: the disk didn't keep up",
                  qPrintable(recorder.path()),
                  static_cast<unsigned long long>(recorder.droppedFrames()));
    }
    qCInfo(lcRecorder, "stopped %s after %.1f s", qPrintable(recorder.path()), recorder.seconds());
    m_recordings.erase(it);
    Q_EMIT filesChanged();
}

std::map<std::string, QString> RecordingManager::recording() const
{
    std::map<std::string, QString> out;
    for (const auto &[key, r] : m_recordings) {
        out[key] = r.recorder->path();
    }
    return out;
}

std::map<std::string, double> RecordingManager::recordedSeconds() const
{
    std::map<std::string, double> out;
    for (const auto &[key, r] : m_recordings) {
        out[key] = r.recorder->seconds();
    }
    return out;
}

bool RecordingManager::play(const QString &path, QString *error)
{
    stopPlayback();
    QString message;
    auto player = Player::open(path, &message);
    if (!player) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    }
    const Properties props{
        {"node.description", "Patch Prime player"},
        {"patchprime.tap.owner", m_buses->owner()},
        {"media.role", "Production"},
        {"media.name", QFileInfo(path).fileName().toStdString()},
    };
    if (!m_graph->addStream(std::string(playerName), false, player->fileRate(), props, player)) {
        if (error != nullptr) {
            *error = QStringLiteral("PipeWire refused the player stream");
        }
        return false;
    }
    m_player = std::move(player);
    updateTaps();
    m_poll.start();
    Q_EMIT changed();
    return true;
}

void RecordingManager::setPaused(bool paused)
{
    if (m_player && m_player->isPaused() != paused) {
        m_player->setPaused(paused);
        Q_EMIT changed();
    }
}

void RecordingManager::stopPlayback()
{
    if (!m_player) {
        return;
    }
    m_graph->removeStream(std::string(playerName));
    m_player.reset();
    updateTaps();
    Q_EMIT changed();
}

void RecordingManager::seek(double seconds)
{
    if (m_player) {
        m_player->seek(seconds);
    }
}

void RecordingManager::setPlayerTarget(const std::string &key)
{
    m_devices->setPlayerTarget(key);
    updateTaps();
}

std::string RecordingManager::playerTarget() const
{
    return m_devices->store().playerTarget();
}

RecordingManager::PlayerState RecordingManager::player() const
{
    if (!m_player) {
        return {};
    }
    return PlayerState{.path = m_player->path(),
                       .paused = m_player->isPaused(),
                       .position = m_player->position(),
                       .duration = m_player->duration()};
}

std::vector<RecordingFile> RecordingManager::files() const
{
    std::vector<RecordingFile> out;
    const QDir dir(m_folder);
    const auto entries =
        dir.entryInfoList({QStringLiteral("*.flac"), QStringLiteral("*.wav"),
                           QStringLiteral("*.mp3"), QStringLiteral("*.ogg")},
                          QDir::Files, QDir::Time);
    for (const auto &info : entries) {
        RecordingFile file{.path = info.absoluteFilePath(),
                           .name = info.fileName(),
                           .bytes = info.size(),
                           .modified = info.lastModified()};
        SF_INFO sf{};
        if (SNDFILE *handle = sf_open(QFile::encodeName(file.path).constData(), SFM_READ, &sf)) {
            // A file still being written reports no length (or a huge one).
            if (sf.samplerate > 0 && sf.frames > 0 && sf.frames < SF_COUNT_MAX / 2) {
                file.seconds = static_cast<double>(sf.frames) / sf.samplerate;
            }
            sf_close(handle);
        }
        out.push_back(std::move(file));
    }
    return out;
}

void RecordingManager::updateTaps()
{
    std::map<std::string, std::string> recorders;
    for (const auto &[key, r] : m_recordings) {
        recorders[r.node] = key;
    }
    m_buses->setTaps(std::move(recorders), m_player ? std::string(playerName) : std::string(),
                     playerTarget());
}

void RecordingManager::poll()
{
    bool any = false;
    for (auto it = m_recordings.begin(); it != m_recordings.end();) {
        if (it->second.recorder->failed()) {
            const auto next = std::next(it);
            finish(it);
            it = next;
            any = true;
        } else {
            ++it;
        }
    }
    if (any) {
        updateTaps();
        Q_EMIT changed();
    }
    if (m_player && m_player->finished()) {
        stopPlayback();
    }
    if (m_recordings.empty() && !m_player) {
        m_poll.stop();
    }
}

} // namespace pp

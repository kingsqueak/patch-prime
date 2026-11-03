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

bool canRecord(EndpointKind kind, Direction direction)
{
    return kind == EndpointKind::VirtualSink || kind == EndpointKind::VirtualInput ||
           (kind == EndpointKind::Hardware && direction == Direction::Input);
}

bool canPlayInto(EndpointKind kind)
{
    return kind == EndpointKind::VirtualSink || kind == EndpointKind::VirtualInput ||
           kind == EndpointKind::Hardware;
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
            finishTake();
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
    finishTake();
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

std::optional<RecordingManager::Source> RecordingManager::source(const std::string &key,
                                                                 QString *error) const
{
    const auto fail = [error](const QString &message) -> std::optional<Source> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    const int row = m_devices->rowForKey(key);
    if (row < 0) {
        return fail(QStringLiteral("no such strip"));
    }
    const auto index = m_devices->index(row);
    const auto kind =
        kindFromName(m_devices->data(index, DeviceModel::KindRole).toString().toStdString());
    const auto busName = m_devices->data(index, DeviceModel::BusNameRole).toString().toStdString();
    const auto direction =
        m_devices->data(index, DeviceModel::DirectionRole).toString() == QLatin1String("input")
            ? Direction::Input
            : Direction::Output;
    if (!canRecord(kind, direction)) {
        return fail(QStringLiteral("only inputs, mix buses and routed inputs can be recorded"));
    }
    if (busName.empty() || !m_graph->isConnected()) {
        return fail(QStringLiteral("the strip isn't running"));
    }
    // A hardware input recorded raw comes straight from the device; any
    // other recording comes after the strip's effects, as late as they make it.
    const bool raw = kind == EndpointKind::Hardware && direction == Direction::Input && recordInputsRaw();
    // A mono input records as it's mixed: one channel.
    return Source{.label = m_devices->data(index, DeviceModel::LabelRole).toString(),
                  .positions = m_buses->stripPositions(key).size() == 1
                                   ? std::vector<std::string>{"MONO"}
                                   : std::vector<std::string>{"FL", "FR"},
                  .lateSeconds = raw ? 0.0 : m_buses->latency(key).seconds};
}

bool RecordingManager::start(const std::string &key, QString *error)
{
    if (m_recordings.contains(key) || (m_take && std::ranges::contains(m_take->keys, key))) {
        return true;
    }
    const auto src = source(key, error);
    if (!src) {
        return false;
    }
    const auto path = uniquePath(
        QDir(m_folder).filePath(recordingFileName(src->label, QDateTime::currentDateTime(), format())));
    // A mono input goes in both of the file's channels.
    auto recorder = std::make_shared<Recorder>(
        std::vector<RecorderTrack>{{.path = path,
                                    .inputChannels = static_cast<int>(src->positions.size()),
                                    .fileChannels = 2,
                                    .lateSeconds = src->lateSeconds}},
        format());
    const int row = m_devices->rowForKey(key);
    const auto busName =
        m_devices->data(m_devices->index(row), DeviceModel::BusNameRole).toString().toStdString();
    const auto node = std::string(recorderPrefix) +
                      (busName.starts_with("patchprime.") ? busName.substr(11) : busName);
    const Properties props{
        {"node.description", "Patch Prime recorder (" + src->label.toStdString() + ")"},
        {"patchprime.tap.owner", m_buses->owner()},
        // Its own class, so Plasma and WirePlumber leave it alone (like sends).
        {"media.class", "Audio/Patchprime/Recorder"},
    };
    if (!m_graph->addStream(node, true, 0, props, recorder, src->positions)) {
        recorder->stop();
        if (error != nullptr) {
            *error = QStringLiteral("PipeWire refused the recording stream");
        }
        return false;
    }
    qCInfo(lcRecorder, "recording %s to %s", key.c_str(), qPrintable(path));
    m_recordings[key] = Recording{.node = node, .recorder = std::move(recorder), .keys = {key}, .folder = {}};
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
    } else if (m_take && std::ranges::contains(m_take->keys, key)) {
        stopTake(); // a take stops as a whole
    }
}

void RecordingManager::close(Recording &recording)
{
    m_graph->removeStream(recording.node);
    auto &recorder = *recording.recorder;
    recorder.stop();
    if (recorder.failed()) {
        m_lastError = QFileInfo(recorder.path()).fileName() + QStringLiteral(": ") + recorder.error();
    } else if (recorder.droppedFrames() > 0) {
        qCWarning(lcRecorder, "%s lost %llu frames: the disk didn't keep up",
                  qPrintable(recorder.path()),
                  static_cast<unsigned long long>(recorder.droppedFrames()));
    }
    qCInfo(lcRecorder, "stopped %s after %.1f s",
           qPrintable(recording.folder.isEmpty() ? recorder.path() : recording.folder), recorder.seconds());
    Q_EMIT filesChanged();
}

void RecordingManager::finish(std::map<std::string, Recording>::iterator it)
{
    close(it->second);
    m_recordings.erase(it);
}

void RecordingManager::finishTake()
{
    if (m_take) {
        close(*m_take);
        m_take.reset();
    }
}

bool RecordingManager::multitrack() const
{
    return m_devices->store().multitrack();
}

void RecordingManager::setMultitrack(bool on)
{
    m_devices->setMultitrack(on);
}

std::vector<std::string> RecordingManager::armed() const
{
    // In mixer order, only strips that still exist.
    std::vector<std::string> out;
    const auto &keys = m_devices->store().armed();
    for (int row = 0; row < m_devices->rowCount(); ++row) {
        const auto key = m_devices->data(m_devices->index(row), DeviceModel::KeyRole).toString().toStdString();
        if (std::ranges::contains(keys, key)) {
            out.push_back(key);
        }
    }
    return out;
}

void RecordingManager::setArmed(const std::string &key, bool armed)
{
    m_devices->setArmed(key, armed);
}

bool RecordingManager::recordInputsRaw() const
{
    return m_devices->store().recordInputsRaw();
}

void RecordingManager::setRecordInputsRaw(bool raw)
{
    m_devices->setRecordInputsRaw(raw);
    if (m_take || !m_recordings.empty()) {
        updateTaps(); // relinks the inputs at once
    }
}

bool RecordingManager::startTake(QString *error)
{
    if (m_take) {
        return true;
    }
    const auto when = QDateTime::currentDateTime();
    const auto folder = uniquePath(QDir(m_folder).filePath(takeFolderName(when)));
    std::vector<RecorderTrack> tracks;
    std::vector<std::string> keys;
    std::vector<std::string> positions;
    QStringList skipped;
    for (const auto &key : armed()) {
        if (m_recordings.contains(key)) {
            stop(key); // recorded alone until now: it joins the take
        }
        QString why;
        const auto src = source(key, &why);
        if (!src) {
            skipped << why;
            continue;
        }
        if (positions.size() + src->positions.size() > Recorder::maxChannels) {
            skipped << QStringLiteral("too many strips");
            break;
        }
        const auto channels = static_cast<int>(src->positions.size());
        tracks.push_back(RecorderTrack{
            .path = uniquePath(QDir(folder).filePath(takeTrackFileName(src->label, format()))),
            .inputChannels = channels,
            .fileChannels = channels,
            .lateSeconds = src->lateSeconds});
        keys.push_back(key);
        positions.insert(positions.end(), src->positions.begin(), src->positions.end());
    }
    if (tracks.empty()) {
        if (error != nullptr) {
            *error = skipped.isEmpty() ? QStringLiteral("no strips are armed")
                                       : QStringLiteral("nothing armed can be recorded: ") + skipped.join(u"; ");
        }
        return false;
    }
    auto recorder = std::make_shared<Recorder>(std::move(tracks), format());
    const Properties props{
        {"node.description", "Patch Prime recorder (take)"},
        {"patchprime.tap.owner", m_buses->owner()},
        {"media.class", "Audio/Patchprime/Recorder"},
    };
    if (!m_graph->addStream(std::string(takeName), true, 0, props, recorder, positions)) {
        recorder->stop();
        if (error != nullptr) {
            *error = QStringLiteral("PipeWire refused the recording stream");
        }
        return false;
    }
    qCInfo(lcRecorder, "recording a take of %zu strips to %s", keys.size(), qPrintable(folder));
    m_take = Recording{.node = std::string(takeName), .recorder = std::move(recorder), .keys = std::move(keys),
                       .folder = folder};
    m_lastError.clear();
    updateTaps();
    m_poll.start();
    Q_EMIT changed();
    return true;
}

void RecordingManager::stopTake()
{
    if (m_take) {
        finishTake();
        updateTaps();
        Q_EMIT changed();
    }
}

QString RecordingManager::takeFolder() const
{
    return m_take ? m_take->folder : QString();
}

double RecordingManager::takeSeconds() const
{
    return m_take ? m_take->recorder->seconds() : 0.0;
}

std::map<std::string, QString> RecordingManager::recording() const
{
    std::map<std::string, QString> out;
    for (const auto &[key, r] : m_recordings) {
        out[key] = r.recorder->path();
    }
    if (m_take) {
        for (std::size_t i = 0; i < m_take->keys.size(); ++i) {
            out[m_take->keys[i]] = m_take->recorder->tracks().at(i).path;
        }
    }
    return out;
}

std::map<std::string, double> RecordingManager::recordedSeconds() const
{
    std::map<std::string, double> out;
    for (const auto &[key, r] : m_recordings) {
        out[key] = r.recorder->seconds();
    }
    if (m_take) {
        for (const auto &key : m_take->keys) {
            out[key] = m_take->recorder->seconds();
        }
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
    player->setLoop(m_loop);
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

void RecordingManager::setLoop(bool loop)
{
    if (loop == m_loop) {
        return;
    }
    m_loop = loop;
    if (m_player) {
        m_player->setLoop(loop);
    }
    Q_EMIT changed();
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
    const QStringList patterns{QStringLiteral("*.flac"), QStringLiteral("*.wav"),
                               QStringLiteral("*.mp3"), QStringLiteral("*.ogg")};
    const auto add = [&out](const QFileInfo &info, const QString &take) {
        RecordingFile file{.path = info.absoluteFilePath(),
                           .name = info.fileName(),
                           .take = take,
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
    };
    const QDir dir(m_folder);
    for (const auto &info : dir.entryInfoList(patterns, QDir::Files)) {
        add(info, {});
    }
    // Takes: a folder each.
    for (const auto &takeDir : dir.entryInfoList({QStringLiteral("Take *")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        for (const auto &info : QDir(takeDir.absoluteFilePath()).entryInfoList(patterns, QDir::Files, QDir::Name)) {
            add(info, takeDir.fileName());
        }
    }
    // Newest first; a take's files together (as new as its newest), by name.
    std::map<QString, QDateTime> takeTime;
    for (const auto &f : out) {
        if (!f.take.isEmpty()) {
            takeTime[f.take] = std::max(takeTime[f.take], f.modified);
        }
    }
    const auto when = [&takeTime](const RecordingFile &f) {
        return f.take.isEmpty() ? f.modified : takeTime[f.take];
    };
    std::ranges::stable_sort(out, [&when](const RecordingFile &a, const RecordingFile &b) {
        if (!a.take.isEmpty() && a.take == b.take) {
            return a.name < b.name;
        }
        const auto ta = when(a);
        const auto tb = when(b);
        return ta != tb ? ta > tb : a.take < b.take;
    });
    return out;
}

void RecordingManager::updateTaps()
{
    std::map<std::string, std::vector<BusManager::TapTrack>> recorders;
    for (const auto &[key, r] : m_recordings) {
        recorders[r.node] = {{.key = key,
                              .channels = static_cast<std::size_t>(r.recorder->tracks().front().inputChannels),
                              .raw = recordInputsRaw()}};
    }
    if (m_take) {
        auto &tracks = recorders[m_take->node];
        for (std::size_t i = 0; i < m_take->keys.size(); ++i) {
            tracks.push_back({.key = m_take->keys[i],
                              .channels = static_cast<std::size_t>(m_take->recorder->tracks().at(i).inputChannels),
                              .raw = recordInputsRaw()});
        }
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
    if (m_take && m_take->recorder->failed()) {
        finishTake();
        any = true;
    }
    if (any) {
        updateTaps();
        Q_EMIT changed();
    }
    if (m_player && m_player->finished()) {
        stopPlayback();
    }
    if (m_recordings.empty() && !m_take && !m_player) {
        m_poll.stop();
    }
}

} // namespace pp

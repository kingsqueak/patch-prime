// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "recording.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pp {

class BusManager;
class DeviceModel;
class PipeWireGraph;

// Strips that can be recorded: mix buses, routed inputs (Desktop), and
// hardware inputs (recorded raw, before their effects).
bool canRecord(EndpointKind kind, Direction direction);
// Strips the player can play into: anything apps play into (mix buses,
// routed inputs, hardware outputs), and hardware inputs (standing in for
// the device, so its effects can be set up on a recording).
bool canPlayInto(EndpointKind kind);

// A file in the recordings folder.
struct RecordingFile
{
    QString path;
    QString name;
    QString take; // the take's folder name, for a track of a multitrack take
    double seconds = 0.0; // 0 when unknown
    qint64 bytes = 0;
    QDateTime modified;
};

// Records mix buses to files and plays files into a strip. Each recording
// is a capture stream (patchprime.record.<bus>) linked to its strip's
// output; the player is one playback stream (patchprime.player) linked into
// its target strip's bus. BusManager makes the links.
class RecordingManager : public QObject
{
    Q_OBJECT

public:
    // folder: where recordings go; empty means ~/Music/Patch Prime.
    RecordingManager(PipeWireGraph *graph, DeviceModel *devices, BusManager *buses,
                     QString folder = {}, QObject *parent = nullptr);
    ~RecordingManager() override;
    RecordingManager(const RecordingManager &) = delete;
    RecordingManager &operator=(const RecordingManager &) = delete;
    RecordingManager(RecordingManager &&) = delete;
    RecordingManager &operator=(RecordingManager &&) = delete;

    static QString defaultFolder();
    [[nodiscard]] const QString &folder() const { return m_folder; }
    [[nodiscard]] RecordingFormat format() const;
    void setFormat(RecordingFormat format);

    // Starts recording the strip with this key; false with error set when it
    // can't (not a mix bus, not enabled, PipeWire refused).
    bool start(const std::string &key, QString *error = nullptr);
    void stop(const std::string &key);
    // Multitrack takes: the armed strips (in mixer order) recorded at once,
    // one file each in a folder of their own ("Take <date time>"), all from
    // one stream so they start on the same sample. startTake() returns false
    // with error set when nothing armed can be recorded.
    [[nodiscard]] bool multitrack() const;
    void setMultitrack(bool on);
    [[nodiscard]] std::vector<std::string> armed() const;
    void setArmed(const std::string &key, bool armed);
    // Hardware inputs recorded raw (before their effects and fader) or as
    // processed (after them), alone or in a take; applies at once.
    [[nodiscard]] bool recordInputsRaw() const;
    void setRecordInputsRaw(bool raw);
    bool startTake(QString *error = nullptr);
    void stopTake();
    [[nodiscard]] bool takeRunning() const { return m_take.has_value(); }
    // The running take's folder (empty when none).
    [[nodiscard]] QString takeFolder() const;
    [[nodiscard]] double takeSeconds() const;

    // Strip key -> the file it's recording to (a take's strips too).
    [[nodiscard]] std::map<std::string, QString> recording() const;
    // Strip key -> seconds recorded so far.
    [[nodiscard]] std::map<std::string, double> recordedSeconds() const;
    // The last recording that failed, and why (empty when none did).
    [[nodiscard]] const QString &lastError() const { return m_lastError; }

    // Plays path into the player's target (see setPlayerTarget).
    bool play(const QString &path, QString *error = nullptr);
    void setPaused(bool paused);
    void stopPlayback();
    void seek(double seconds);
    void setPlayerTarget(const std::string &key);
    // Plays the file over and over (for this and later files).
    void setLoop(bool loop);
    [[nodiscard]] bool isLooping() const { return m_loop; }
    [[nodiscard]] std::string playerTarget() const;
    struct PlayerState
    {
        QString path; // empty when stopped
        bool paused = false;
        double position = 0.0;
        double duration = 0.0;
    };
    [[nodiscard]] PlayerState player() const;

    // The recordings folder's sound files, newest first.
    [[nodiscard]] std::vector<RecordingFile> files() const;

    static constexpr std::string_view recorderPrefix = "patchprime.record.";
    static constexpr std::string_view playerName = "patchprime.player";
    static constexpr std::string_view takeName = "patchprime.record.take";

Q_SIGNALS:
    // A recording started or stopped, the player started, paused or stopped,
    // or the settings changed. Not sent for times moving on.
    void changed();
    // The folder's contents changed (a recording finished).
    void filesChanged();

private:
    struct Recording
    {
        std::string node;
        std::shared_ptr<Recorder> recorder;
        std::vector<std::string> keys; // its tracks' strips, in order
        QString folder;                // a take's
    };
    // A strip that can be recorded now: its label and channel positions.
    struct Source
    {
        QString label;
        std::vector<std::string> positions;
        // How late its sound reaches the recorder: its effects' latency,
        // unless it's recorded raw (see RecorderTrack::lateSeconds).
        double lateSeconds = 0.0;
    };
    std::optional<Source> source(const std::string &key, QString *error) const;
    void close(Recording &recording);
    void finish(std::map<std::string, Recording>::iterator it);
    void finishTake();
    void updateTaps();
    void poll();

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    BusManager *m_buses;
    QString m_folder;
    QString m_lastError;
    std::map<std::string, Recording> m_recordings; // single strips, by key
    std::optional<Recording> m_take;
    std::shared_ptr<Player> m_player;
    bool m_loop = false;
    QTimer m_poll;
};

} // namespace pp

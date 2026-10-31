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
#include <string>
#include <vector>

namespace pp {

class BusManager;
class DeviceModel;
class PipeWireGraph;

// Strips that can be recorded: mix buses and routed inputs (Desktop).
bool canRecord(EndpointKind kind);
// Strips the player can play into: anything apps play into (mix buses,
// routed inputs, hardware outputs).
bool canPlayInto(EndpointKind kind, Direction direction);

// A file in the recordings folder.
struct RecordingFile
{
    QString path;
    QString name;
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
    // Strip key -> the file it's recording to.
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
    };
    void finish(std::map<std::string, Recording>::iterator it);
    void updateTaps();
    void poll();

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    BusManager *m_buses;
    QString m_folder;
    QString m_lastError;
    std::map<std::string, Recording> m_recordings; // by strip key
    std::shared_ptr<Player> m_player;
    QTimer m_poll;
};

} // namespace pp

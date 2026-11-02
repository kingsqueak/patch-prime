// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "loudnessmeter.h"

#include <QObject>
#include <QTimer>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pp {

class BusManager;
class DeviceModel;
class PipeWireGraph;
class RecordingManager;

// Loudness for publishing on one strip the user picks (devices.json
// loudnessStrip): a capture stream, patchprime.loudness, linked after the
// strip's effects and fader (what it sends on), measured continuously
// whether or not anyone is looking. It starts over on reset(), when the
// strip changes, and when the strip starts recording (alone or in a take),
// so the reading matches the file.
class LoudnessManager : public QObject
{
    Q_OBJECT

public:
    LoudnessManager(PipeWireGraph *graph, DeviceModel *devices, BusManager *buses,
                    RecordingManager *recordings = nullptr, QObject *parent = nullptr);
    ~LoudnessManager() override;
    LoudnessManager(const LoudnessManager &) = delete;
    LoudnessManager &operator=(const LoudnessManager &) = delete;
    LoudnessManager(LoudnessManager &&) = delete;
    LoudnessManager &operator=(LoudnessManager &&) = delete;

    static constexpr std::string_view nodeName = "patchprime.loudness";

    [[nodiscard]] std::string strip() const;
    void setStrip(const std::string &key);
    [[nodiscard]] double target() const;
    [[nodiscard]] double peakTarget() const;
    void setTargets(double lufs, double dbtp);
    void reset();
    // Whether the strip is being measured now (it exists and is running).
    [[nodiscard]] bool measuring() const { return m_meter != nullptr; }
    [[nodiscard]] const LoudnessAnalysis &analysis() const { return m_analysis; }

Q_SIGNALS:
    // The strip, the targets or whether it's measuring changed (not the readings).
    void changed();

private:
    void update();
    void stopMeter();
    void poll();
    void onRecordingsChanged();

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    BusManager *m_buses;
    RecordingManager *m_recordings;
    std::shared_ptr<LoudnessMeter> m_meter;
    std::string m_measured;                // the strip the meter is on
    std::vector<std::string> m_positions;  // its channels then
    LoudnessAnalysis m_analysis;
    std::set<std::string> m_wasRecording;
    QTimer m_poll;
    QTimer m_updateTimer;
};

} // namespace pp

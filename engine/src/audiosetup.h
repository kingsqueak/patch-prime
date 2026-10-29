// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <map>
#include <string>

namespace pp {

class DeviceModel;
class PipeWireGraph;

// --- Pure helpers (unit tested) ---------------------------------------------

// PipeWire's runtime settings for the mixer's rate and block size: the graph
// runs at that rate only (clock.rate, clock.allowed-rates), and at a fixed
// quantum, or PipeWire's choice for block size 0 (clock.force-quantum).
std::map<std::string, std::string> wantedSettings(const DeviceStore &store);

// The WirePlumber rules for the per-device settings (scheduling, a device's
// own sample rate), as a wireplumber.conf.d fragment. Auto scheduling means
// timer scheduling while Patch Prime is the clock: an interface that
// follows another clock on interrupts can't rate-match smoothly, and its
// audio pulses (found with a Scarlett 8i6 next to a Fast Track).
std::string wirePlumberRules(const DeviceStore &store);

// --- AudioSetup --------------------------------------------------------------

// Keeps PipeWire's settings and Patch Prime's WirePlumber rule file in line
// with the store. Settings are written whenever they differ (after a PipeWire
// restart, say). The rule file only changes when a setting does, and then
// WirePlumber is restarted so it applies (a few seconds' dropout).
class AudioSetup : public QObject
{
    Q_OBJECT

public:
    // rulesPath: empty for ~/.config/wireplumber/wireplumber.conf.d/
    // 60-patchprime.conf. Tests pass their own path and no restart.
    AudioSetup(PipeWireGraph *graph, DeviceModel *devices, QString rulesPath = {},
               bool restartWirePlumber = true, QObject *parent = nullptr);

    static QString defaultRulesPath();
    [[nodiscard]] const QString &rulesPath() const { return m_rulesPath; }

    // Writes the rule file now if it's out of date; true when it was written.
    bool writeRules();

private:
    void scheduleApply();
    void applySettings();

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    QString m_rulesPath;
    bool m_restartWirePlumber;
    QTimer m_applyTimer;
};

} // namespace pp

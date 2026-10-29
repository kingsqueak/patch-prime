// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

namespace pp {
class BusManager;
class DeviceModel;
class PipeWireGraph;
} // namespace pp

// The daemon's D-Bus API, org.kingsqueak.PatchPrime1 at /org/kingsqueak/PatchPrime.
//
// GetState() and StateChanged carry JSON: {"connected": bool, "devices": [..]},
// one object per device whose fields are DeviceModel's role names. Devices are
// addressed by their key (media.class|node.name).
//
// Levels(a{sv}) maps a device key to its per-channel peaks (ad, linear) about
// 30 times a second, only while at least one client has called WatchLevels().
class PatchPrimeService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kingsqueak.PatchPrime1")

public:
    static constexpr auto serviceName = "org.kingsqueak.PatchPrime";
    static constexpr auto objectPath = "/org/kingsqueak/PatchPrime";

    PatchPrimeService(pp::PipeWireGraph *graph, pp::DeviceModel *devices, pp::BusManager *buses,
                      QObject *parent = nullptr);

public Q_SLOTS:
    QString GetState() const;
    void SetEnabled(const QString &key, bool enabled);
    void SetLabel(const QString &key, const QString &label);
    void SetVolume(const QString &key, double volumeDb);
    void SetMuted(const QString &key, bool muted);
    void Forget(const QString &key);
    // throughMixer: make this output's bus the system default output, so apps
    // that follow the default play through the mixer. Otherwise make the
    // hardware output itself the default.
    void SetDefaultOutput(const QString &key, bool throughMixer);
    void SetMono(const QString &key, bool mono);
    // Sends an input to an output or virtual device, or stops sending it.
    void SetSend(const QString &inputKey, const QString &destinationKey, bool enabled);
    // kind: "sink" (apps play into it; record its monitor, e.g. "Stream Mix")
    // or "source" (apps record from it like a microphone, e.g. "Chat Mic").
    // Returns the new device's key. Rename with SetLabel, delete with Forget.
    QString CreateVirtual(const QString &name, const QString &kind);
    void WatchLevels();
    void UnwatchLevels();

Q_SIGNALS:
    void StateChanged(const QString &state);
    void Levels(const QVariantMap &levels);

private:
    [[nodiscard]] int rowFor(const QString &key);
    void scheduleState();
    void updateMetering();
    void emitLevels();

    pp::PipeWireGraph *m_graph;
    pp::DeviceModel *m_devices;
    pp::BusManager *m_buses;
    QTimer m_stateTimer;
    QTimer m_levelsTimer;
    QDBusServiceWatcher m_watcher;
    QSet<QString> m_levelWatchers;
};

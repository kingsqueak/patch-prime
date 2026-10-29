// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QAbstractListModel>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QVariantMap>

class QDBusPendingCallWatcher;

// The GUI's view of patchprimed: its device list as a model, fed by the
// StateChanged and Levels D-Bus signals. Edits go to the daemon, and the
// change comes back in the next state, so the daemon stays the only owner.
class DaemonClient : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool daemonRunning READ isDaemonRunning NOTIFY daemonRunningChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(double minimumVolumeDb READ minimumVolumeDb CONSTANT)
    Q_PROPERTY(double maximumVolumeDb READ maximumVolumeDb CONSTANT)
    // Applications: [{key, name, icon, direction, target, streams}], see the daemon's state.
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    // What clocks the audio graph: "internal" or "hardware".
    Q_PROPERTY(QString clock READ clock NOTIFY clockChanged)
    // The mixer's sample rate (Hz) and block size (frames, 0 = automatic).
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY clockChanged)
    Q_PROPERTY(int blockSize READ blockSize NOTIFY clockChanged)
    // Where apps can play to / record from: [{name (a node.name), label, kind}].
    Q_PROPERTY(QVariantList playbackTargets READ playbackTargets NOTIFY appsChanged)
    Q_PROPERTY(QVariantList recordTargets READ recordTargets NOTIFY appsChanged)

public:
    explicit DaemonClient(QObject *parent = nullptr);
    ~DaemonClient() override;
    DaemonClient(const DaemonClient &) = delete;
    DaemonClient &operator=(const DaemonClient &) = delete;
    DaemonClient(DaemonClient &&) = delete;
    DaemonClient &operator=(DaemonClient &&) = delete;

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] bool isDaemonRunning() const { return m_daemonRunning; }
    [[nodiscard]] bool isConnected() const { return m_connected; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] QVariantList apps() const { return m_apps; }
    [[nodiscard]] QString clock() const { return m_clock; }
    [[nodiscard]] int sampleRate() const { return m_sampleRate; }
    [[nodiscard]] int blockSize() const { return m_blockSize; }
    [[nodiscard]] QVariantList playbackTargets() const { return m_playbackTargets; }
    [[nodiscard]] QVariantList recordTargets() const { return m_recordTargets; }
    static constexpr double minimumVolumeDb() { return -60.0; }
    static constexpr double maximumVolumeDb() { return 12.0; }

    Q_INVOKABLE void setEnabled(const QString &key, bool enabled);
    Q_INVOKABLE void setLabel(const QString &key, const QString &label);
    Q_INVOKABLE void setVolume(const QString &key, double volumeDb);
    Q_INVOKABLE void setMuted(const QString &key, bool muted);
    Q_INVOKABLE void forget(const QString &key);
    Q_INVOKABLE void setDefaultOutput(const QString &key, bool throughMixer);
    Q_INVOKABLE void setMono(const QString &key, bool mono);
    Q_INVOKABLE void setChannelPair(const QString &key, int pair);
    // A strip's label by key; empty when there's no such strip.
    [[nodiscard]] Q_INVOKABLE QString labelFor(const QString &key) const;
    Q_INVOKABLE void setSend(const QString &inputKey, const QString &destinationKey, bool enabled);
    Q_INVOKABLE void setSendLevel(const QString &inputKey, const QString &destinationKey,
                                  double db);
    // kind: "sink", "source" or "input"
    Q_INVOKABLE void createVirtual(const QString &name, const QString &kind);
    // nodeName: a target's name; empty for the system default.
    Q_INVOKABLE void setAppTarget(const QString &appKey, const QString &nodeName);
    Q_INVOKABLE void forgetApp(const QString &appKey);
    Q_INVOKABLE void setClock(const QString &mode);
    Q_INVOKABLE void setSampleRate(int rate);
    Q_INVOKABLE void setBlockSize(int frames);
    Q_INVOKABLE void setDeviceSampleRate(const QString &key, int rate);
    Q_INVOKABLE void setDeviceScheduling(const QString &key, const QString &scheduling);
    // D-Bus activation first; failing that, a patchprimed next to this program
    // (installed side by side, or in the build tree).
    Q_INVOKABLE void startDaemon();

    // Meter fraction 0..1 for a linear peak, on a dB scale from minimumVolumeDb to 0.
    static double meterFraction(double peak);

Q_SIGNALS:
    void daemonRunningChanged();
    void connectedChanged();
    void lastErrorChanged();
    void appsChanged();
    void clockChanged();

private Q_SLOTS:
    void onStateChanged(const QString &state);
    void onLevels(const QVariantMap &levels);

private: // NOLINT(readability-redundant-access-specifiers): ends the Q_SLOTS section
    void setDaemonRunning(bool running);
    void fetchState();
    void call(const QString &method, const QVariantList &args);
    void setLastError(const QString &error);

    QHash<int, QByteArray> m_roles;
    QHash<QByteArray, int> m_roleIds;
    int m_levelsRole = 0;
    int m_signalRole = 0;
    QList<QVariantMap> m_rows;
    QHash<QString, QList<double>> m_levels; // key -> displayed meter fractions
    QHash<QString, int> m_signalHold;       // key -> updates left showing "signal present"
    QDBusServiceWatcher m_watcher;
    bool m_daemonRunning = false;
    bool m_connected = false;
    QString m_lastError;
    QVariantList m_apps;
    QString m_clock;
    int m_sampleRate = 0;
    int m_blockSize = 0;
    QVariantList m_playbackTargets;
    QVariantList m_recordTargets;
};

// The enabled strips of some stages ("input", "bus", "output"), for a row of
// mixer strips or one side of the routing grid; in console order (inputs,
// mix buses, outputs), otherwise as the daemon lists them.
class StripFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QStringList stages READ stages WRITE setStages NOTIFY stagesChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit StripFilter(QObject *parent = nullptr);

    [[nodiscard]] int count() const { return rowCount(); }

    [[nodiscard]] QStringList stages() const { return m_stages; }
    void setStages(const QStringList &stages);

Q_SIGNALS:
    void stagesChanged();
    void countChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex &sourceParent) const override;
    [[nodiscard]] bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QStringList m_stages;
};

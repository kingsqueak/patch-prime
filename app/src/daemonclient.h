// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QAbstractListModel>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QDateTime>
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
    // With the hardware clock: the chosen device's key. clockNow: the device
    // that actually clocks the graph right now.
    Q_PROPERTY(QString clockDevice READ clockDevice NOTIFY clockChanged)
    Q_PROPERTY(QString clockNow READ clockNow NOTIFY clockChanged)
    // What can be the clock: [{value, text}], "internal" first, then the
    // enabled hardware devices by key.
    Q_PROPERTY(QVariantList clockChoices READ clockChoices NOTIFY clockChanged)
    // Saved scenes (names), and the one last saved or loaded.
    Q_PROPERTY(QStringList scenes READ scenes NOTIFY scenesChanged)
    Q_PROPERTY(QString currentScene READ currentScene NOTIFY scenesChanged)
    // The mixer's sample rate (Hz) and block size (frames, 0 = automatic).
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY clockChanged)
    Q_PROPERTY(int blockSize READ blockSize NOTIFY clockChanged)
    // The hardware output cue plays to (a strip key; empty: cue is off).
    Q_PROPERTY(QString cueOutput READ cueOutput NOTIFY cueOutputChanged)
    // The Mackie Control surface: {id, available, connected, ports: [{id, name}]}.
    Q_PROPERTY(QVariantMap controlSurface READ controlSurface NOTIFY controlSurfaceChanged)
    // MIDI learn: {id, connected} of the controller's port; the mappings
    // ([{id, kind, channel, number, relative, target, control, description}]);
    // the target being learned (null when none) and what it is.
    Q_PROPERTY(QVariantMap midiController READ midiController NOTIFY midiChanged)
    Q_PROPERTY(QVariantList midiMappings READ midiMappings NOTIFY midiChanged)
    Q_PROPERTY(QVariant midiLearning READ midiLearning NOTIFY midiChanged)
    Q_PROPERTY(QString midiLearningText READ midiLearningText NOTIFY midiChanged)
    // Global hotkeys: [{id, mode, target, description, keys}], and whether
    // the desktop has global shortcuts at all.
    Q_PROPERTY(QVariantList hotkeys READ hotkeys NOTIFY hotkeysChanged)
    Q_PROPERTY(bool hotkeysAvailable READ hotkeysAvailable NOTIFY hotkeysChanged)
    // Built-in effects on offer: [{type, label, params: [{name, label, unit,
    // minimum, maximum, initial}]}]. A strip's chain is its "effects" role.
    Q_PROPERTY(QVariantList effectTypes READ effectTypes NOTIFY effectTypesChanged)
    // The installed plugins (see the daemon's GetPlugins): [{type, name,
    // author, category, audioIn, audioOut, params, problem}], by name.
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsScanning READ pluginsScanning NOTIFY pluginsChanged)
    // How far a scan is: {done, total, found} (files read of all, plugins
    // found); and when the list was made (empty for never).
    Q_PROPERTY(QVariantMap pluginsScan READ pluginsScan NOTIFY pluginsChanged)
    Q_PROPERTY(QDateTime pluginsLastScan READ pluginsLastScan NOTIFY pluginsChanged)
    // Plugins that are off because they failed: {stripKey: {index: message}}.
    Q_PROPERTY(QVariantMap pluginErrors READ pluginErrors NOTIFY pluginsChanged)
    // Plugins' own windows: open ones, {stripKey: [index]}; and why ones
    // couldn't be opened, {stripKey: {index: message}}.
    Q_PROPERTY(QVariantMap pluginWindows READ pluginWindows NOTIFY pluginsChanged)
    Q_PROPERTY(QVariantMap pluginWindowErrors READ pluginWindowErrors NOTIFY pluginsChanged)
    // Effects' meters by "<strip key>|<effect index>": a list of values
    // (the multiband compressor's gain reduction per band in dB), each held
    // and let fall slowly so it can be read.
    Q_PROPERTY(QVariantMap effectMeters READ effectMeters NOTIFY effectMetersChanged)
    // "<strip key>|<effect index>" -> {in, inRms, out, outRms}: meter
    // fractions on the effectMeterFloorDb scale, one per channel group (mono,
    // or left and right); and key, keyRms (one each) on a keyed strip.
    Q_PROPERTY(QVariantMap effectLevels READ effectLevels NOTIFY effectMetersChanged)
    Q_PROPERTY(double effectMeterFloorDb READ effectMeterFloorDb CONSTANT)
    // Where apps can play to / record from: [{name (a node.name), label, kind}].
    Q_PROPERTY(QVariantList playbackTargets READ playbackTargets NOTIFY appsChanged)
    Q_PROPERTY(QVariantList recordTargets READ recordTargets NOTIFY appsChanged)
    // Recording: the folder, the format ("flac", "wav" or "mp3"), the strips
    // recording now ({key: file}) and for how long ({key: seconds}), and why
    // the last recording failed (empty when it didn't).
    Q_PROPERTY(QString recordingFolder READ recordingFolder NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingFormat READ recordingFormat NOTIFY recordingChanged)
    // The formats the engine can write (MP3 depends on libsndfile's build).
    Q_PROPERTY(QStringList recordingFormats READ recordingFormats NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap recordingSeconds READ recordingSeconds NOTIFY recordingTimesChanged)
    Q_PROPERTY(QString recordingError READ recordingError NOTIFY recordingChanged)
    // Goes up when the folder's files change (call recordings() again).
    Q_PROPERTY(int recordingsRevision READ recordingsRevision NOTIFY recordingChanged)
    // The player: {path (empty when stopped), paused, duration, loop}, where it
    // is (seconds), and the strip it plays into (a key).
    Q_PROPERTY(QVariantMap player READ player NOTIFY recordingChanged)
    Q_PROPERTY(double playerPosition READ playerPosition NOTIFY recordingTimesChanged)
    Q_PROPERTY(QString playerTarget READ playerTarget NOTIFY recordingChanged)

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
    [[nodiscard]] QString clockDevice() const { return m_clockDevice; }
    [[nodiscard]] QString clockNow() const { return m_clockNow; }
    [[nodiscard]] QVariantList clockChoices() const { return m_clockChoices; }
    [[nodiscard]] QStringList scenes() const { return m_scenes; }
    [[nodiscard]] QString currentScene() const { return m_currentScene; }
    [[nodiscard]] int sampleRate() const { return m_sampleRate; }
    [[nodiscard]] int blockSize() const { return m_blockSize; }
    [[nodiscard]] QVariantList effectTypes() const { return m_effectTypes; }
    [[nodiscard]] QVariantList plugins() const { return m_plugins; }
    [[nodiscard]] bool pluginsScanning() const { return m_pluginsScanning; }
    [[nodiscard]] QVariantMap pluginsScan() const { return m_pluginsScan; }
    [[nodiscard]] QDateTime pluginsLastScan() const { return m_pluginsLastScan; }
    [[nodiscard]] QVariantMap pluginErrors() const { return m_pluginErrors; }
    [[nodiscard]] QVariantMap pluginWindows() const { return m_pluginWindows; }
    [[nodiscard]] QVariantMap pluginWindowErrors() const { return m_pluginWindowErrors; }
    Q_INVOKABLE void setPluginWindow(const QString &key, int index, bool open);
    // An effect type's name and parameters: a built-in's (from effectTypes)
    // or a plugin's; {type, label, params} either way.
    [[nodiscard]] Q_INVOKABLE QVariantMap effectInfo(const QString &type) const;
    Q_INVOKABLE void rescanPlugins();
    [[nodiscard]] QString cueOutput() const { return m_cueOutput; }
    [[nodiscard]] QVariantMap controlSurface() const { return m_controlSurface; }
    // The control surface's MIDI port (an id from controlSurface.ports), or "".
    Q_INVOKABLE void setControlSurface(const QString &id);
    [[nodiscard]] QVariantMap midiController() const { return m_midiController; }
    [[nodiscard]] QVariantList midiMappings() const { return m_midiMappings; }
    [[nodiscard]] QVariant midiLearning() const { return m_midiLearning; }
    [[nodiscard]] QString midiLearningText() const { return m_midiLearningText; }
    Q_INVOKABLE void setMidiController(const QString &id);
    // target: {action, key, destination, effect, param, scene} (see the
    // daemon's StartMidiLearn).
    Q_INVOKABLE void startMidiLearn(const QVariantMap &target);
    Q_INVOKABLE void cancelMidiLearn();
    Q_INVOKABLE void removeMidiMapping(const QString &id);
    Q_INVOKABLE void setMidiMappingRelative(const QString &id, bool relative);
    // The mapping for a target, or an empty map.
    [[nodiscard]] Q_INVOKABLE QVariantMap midiMappingFor(const QVariantMap &target) const;
    [[nodiscard]] QVariantList hotkeys() const { return m_hotkeys; }
    [[nodiscard]] bool hotkeysAvailable() const { return m_hotkeysAvailable; }
    // A new hotkey ({mode, target}; see the daemon's AddHotkey) with this
    // key (a QKeySequence; empty for none); returns its id, "" on failure.
    Q_INVOKABLE QString addHotkey(const QVariantMap &hotkey, const QVariant &keySequence);
    Q_INVOKABLE void removeHotkey(const QString &id);
    Q_INVOKABLE void setHotkeyKey(const QString &id, const QVariant &keySequence);
    // A hotkey's key (a Qt key combination) as a QKeySequence, for
    // KeySequenceItem.
    [[nodiscard]] Q_INVOKABLE QVariant keySequence(int key) const;
    // A target for people ("Mic: mute"), asked of the daemon.
    [[nodiscard]] Q_INVOKABLE QString describeTarget(const QVariantMap &target) const;
    [[nodiscard]] QVariantMap effectMeters() const { return m_effectMeters; }
    [[nodiscard]] QVariantMap effectLevels() const { return m_effectLevels; }
    [[nodiscard]] QVariantList playbackTargets() const { return m_playbackTargets; }
    [[nodiscard]] QVariantList recordTargets() const { return m_recordTargets; }
    [[nodiscard]] QString recordingFolder() const { return m_recordingFolder; }
    [[nodiscard]] QString recordingFormat() const { return m_recordingFormat; }
    [[nodiscard]] QStringList recordingFormats() const { return m_recordingFormats; }
    [[nodiscard]] QVariantMap recording() const { return m_recording; }
    [[nodiscard]] QVariantMap recordingSeconds() const { return m_recordingSeconds; }
    [[nodiscard]] QString recordingError() const { return m_recordingError; }
    [[nodiscard]] int recordingsRevision() const { return m_recordingsRevision; }
    [[nodiscard]] QVariantMap player() const { return m_player; }
    [[nodiscard]] double playerPosition() const { return m_playerPosition; }
    [[nodiscard]] QString playerTarget() const { return m_playerTarget; }
    static constexpr double minimumVolumeDb() { return -60.0; }
    // Effect meters reach lower than the strips', down to a mic's noise floor,
    // so a gate's threshold can be set between the floor and the voice.
    static constexpr double effectMeterFloorDb() { return -80.0; }
    static constexpr double maximumVolumeDb() { return 12.0; }

    Q_INVOKABLE void setEnabled(const QString &key, bool enabled);
    Q_INVOKABLE void setLabel(const QString &key, const QString &label);
    Q_INVOKABLE void setVolume(const QString &key, double volumeDb);
    Q_INVOKABLE void setMuted(const QString &key, bool muted);
    Q_INVOKABLE void forget(const QString &key);
    Q_INVOKABLE void setDefaultOutput(const QString &key, bool throughMixer);
    Q_INVOKABLE void setDefaultInput(const QString &key, bool throughMixer);
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
    Q_INVOKABLE void setClockDevice(const QString &key);
    Q_INVOKABLE void saveScene(const QString &name);
    Q_INVOKABLE void loadScene(const QString &name);
    Q_INVOKABLE void deleteScene(const QString &name);
    Q_INVOKABLE void setSampleRate(int rate);
    Q_INVOKABLE void setBlockSize(int frames);
    Q_INVOKABLE void setDeviceSampleRate(const QString &key, int rate);
    Q_INVOKABLE void setDeviceScheduling(const QString &key, const QString &scheduling);
    Q_INVOKABLE void setCueOutput(const QString &key);
    Q_INVOKABLE void setCue(const QString &key, bool cued);
    Q_INVOKABLE void addEffect(const QString &key, const QString &type);
    Q_INVOKABLE void removeEffect(const QString &key, int index);
    Q_INVOKABLE void moveEffect(const QString &key, int from, int to);
    Q_INVOKABLE void setEffectEnabled(const QString &key, int index, bool enabled);
    Q_INVOKABLE void setEffectParam(const QString &key, int index, const QString &name,
                                    double value);
    // While an effects window shows a strip: its effects' input and output
    // levels (effectLevels). Counted, so two windows on one strip both work.
    Q_INVOKABLE void watchEffectLevels(const QString &key, bool watch);
    Q_INVOKABLE void setEffectKey(const QString &key, int index, const QString &sourceKey);
    Q_INVOKABLE void startRecording(const QString &key);
    Q_INVOKABLE void stopRecording(const QString &key);
    Q_INVOKABLE void setRecordingFormat(const QString &format);
    // The recordings folder's files, newest first: [{path, name, seconds,
    // bytes, modified}]. Asks the daemon and waits for the answer.
    [[nodiscard]] Q_INVOKABLE QVariantList recordings() const;
    Q_INVOKABLE void playRecording(const QString &file);
    Q_INVOKABLE void pausePlayback(bool paused);
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE void seekPlayback(double seconds);
    Q_INVOKABLE void setPlayerTarget(const QString &key);
    Q_INVOKABLE void setPlaybackLoop(bool loop);
    // Moves a file to the desktop's trash; false when it can't.
    Q_INVOKABLE bool trashFile(const QString &file);
    // An EQ's response in dB at each frequency in freqs (Hz), from its
    // parameters (an "effects" entry's params; missing ones take their
    // defaults); only band (1-6) when given, else all bands together.
    [[nodiscard]] Q_INVOKABLE QVariantList eqCurve(const QVariantMap &params,
                                                   const QVariantList &freqs, int band = 0) const;
    // D-Bus activation first; failing that, a patchprimed next to this program
    // (installed side by side, or in the build tree).
    Q_INVOKABLE void startDaemon();

    // Meter fraction 0..1 for a linear peak, on a dB scale from minimumVolumeDb to 0.
    static double meterFraction(double peak);
    // The same on the effect meters' scale (effectMeterFloorDb to 0).
    static double effectMeterFraction(double peak);

Q_SIGNALS:
    void daemonRunningChanged();
    void connectedChanged();
    void lastErrorChanged();
    void appsChanged();
    void clockChanged();
    void scenesChanged();
    void effectTypesChanged();
    void pluginsChanged();
    void cueOutputChanged();
    void controlSurfaceChanged();
    void midiChanged();
    void hotkeysChanged();
    void effectMetersChanged();
    void recordingChanged();
    void recordingTimesChanged();

private Q_SLOTS:
    void onStateChanged(const QString &state);
    void onLevels(const QVariantMap &levels);

private: // NOLINT(readability-redundant-access-specifiers): ends the Q_SLOTS section
    void setDaemonRunning(bool running);
    void fetchState();
    void call(const QString &method, const QVariantList &args);
    // For methods that answer with an error message ("" when all went well).
    void callReportingError(const QString &method, const QVariantList &args);
    void setLastError(const QString &error);

    QHash<int, QByteArray> m_roles;
    QHash<QByteArray, int> m_roleIds;
    int m_levelsRole = 0;
    int m_signalRole = 0;
    QList<QVariantMap> m_rows;
    QHash<QString, QList<double>> m_levels; // key -> displayed meter fractions
    QHash<QString, int> m_signalHold;       // key -> updates left showing "signal present"
    struct Hold
    {
        double level = 0.0; // meter fraction
        int updates = 0;    // left before it starts falling
    };
    QHash<QString, QList<Hold>> m_peakHold; // key -> per channel
    QHash<QString, QList<double>> m_rms;    // key -> meter fractions
    QHash<QString, double> m_lufs;          // key -> short-term loudness
    int m_holdRole = 0;
    int m_rmsRole = 0;
    int m_lufsRole = 0;
    QDBusServiceWatcher m_watcher;
    bool m_daemonRunning = false;
    bool m_connected = false;
    QString m_lastError;
    QVariantList m_apps;
    QString m_clock;
    QString m_clockDevice;
    QString m_clockNow;
    QVariantList m_clockChoices;
    QStringList m_scenes;
    QString m_currentScene;
    int m_sampleRate = 0;
    int m_blockSize = 0;
    QVariantList m_effectTypes;
    QVariantList m_plugins;
    QHash<QString, int> m_pluginIndex; // type -> position in m_plugins
    int m_pluginsRevision = -1;
    bool m_pluginsScanning = false;
    QVariantMap m_pluginsScan;
    QDateTime m_pluginsLastScan;
    QVariantMap m_pluginErrors;
    QVariantMap m_pluginWindows;
    QVariantMap m_pluginWindowErrors;
    QString m_cueOutput;
    QVariantMap m_controlSurface;
    QVariantMap m_midiController;
    QVariantList m_midiMappings;
    QVariant m_midiLearning;
    QString m_midiLearningText;
    QVariantList m_hotkeys;
    bool m_hotkeysAvailable = false;
    QVariantMap m_effectMeters;
    QVariantMap m_effectLevels;
    QHash<QString, int> m_effectLevelWatches; // strip key -> windows showing it
    QVariantList m_playbackTargets;
    QVariantList m_recordTargets;
    QString m_recordingFolder;
    QString m_recordingFormat;
    QStringList m_recordingFormats;
    QVariantMap m_recording;
    QVariantMap m_recordingSeconds;
    QString m_recordingError;
    int m_recordingsRevision = 0;
    QVariantMap m_player;
    double m_playerPosition = 0.0;
    QString m_playerTarget;
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
    // A row's roles by name ({key, label, kind, ...}), for QML lists.
    [[nodiscard]] Q_INVOKABLE QVariantMap get(int row) const;

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

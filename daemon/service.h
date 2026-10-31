// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

namespace pp {
class AppRouter;
class SceneStore;
class BusManager;
class DeviceModel;
class PipeWireGraph;
class RecordingManager;
} // namespace pp

// The daemon's D-Bus API, org.kingsqueak.PatchPrime1 at /org/kingsqueak/PatchPrime.
//
// GetState() and StateChanged carry JSON: {"connected": bool, "devices": [..],
// "apps": [..]}. One object per device whose fields are DeviceModel's role
// names; devices are addressed by their key (media.class|node.name). One object
// per application: key ("playback|<binary>" or "record|<binary>"), name, icon,
// direction ("playback" or "record"), target (a node.name, empty for the
// system default) and streams (how many are live; 0 when not running).
//
// Levels(a{sv}) maps a device key to its per-channel peaks (ad, linear) about
// 30 times a second, only while at least one client has called WatchLevels().
// Also "rms|<device key>" -> RMS per channel (ad, linear, ~300 ms) and
// "lufs|<device key>" -> [short-term loudness in LUFS, ~3 s, K-weighted].
// Effects with meters add "fx|<device key>|<effect index>" -> their values
// (ad; the multiband compressor's gain reduction per band, in dB). Every
// effect adds "fxio|<device key>|<effect index>" -> [input peaks, input RMS,
// output peaks, output RMS] (linear, a quarter of the list each: one value
// for a mono strip, left and right otherwise; peaks ad, RMS ~300 ms), then
// on a strip with a key input (a ducker's) [key peak, key RMS].
// While recording or playing: "rec|<device key>" -> [seconds recorded] and
// "player" -> [position in seconds].
class PatchPrimeService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kingsqueak.PatchPrime1")

public:
    static constexpr auto serviceName = "org.kingsqueak.PatchPrime";
    static constexpr auto objectPath = "/org/kingsqueak/PatchPrime";

    PatchPrimeService(pp::PipeWireGraph *graph, pp::DeviceModel *devices, pp::BusManager *buses,
                      pp::AppRouter *apps, pp::SceneStore *scenes, pp::RecordingManager *recordings,
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
    // The same for the system default input (microphone): an input's bus, so
    // apps record it with its fader, mute and mono applied, or the hardware
    // input itself; or a virtual microphone.
    void SetDefaultInput(const QString &key, bool throughMixer);
    void SetMono(const QString &key, bool mono);
    // Hardware with more than two channels: 0 = all, n = stereo pair 2n-1/2n.
    void SetChannelPair(const QString &key, int pair);
    // Sends an input to an output or virtual device, or stops sending it.
    void SetSend(const QString &inputKey, const QString &destinationKey, bool enabled);
    // What clocks the audio graph: "internal" (Patch Prime's buses; unplugging
    // an interface doesn't interrupt the rest) or "hardware" (the interface
    // chosen with SetClockDevice; lowest latency; while it's unplugged, Patch
    // Prime's buses keep time). In the state as "clock".
    void SetClock(const QString &mode);
    // Makes an ALSA interface (by key) the clock; switches to the hardware
    // clock. In the state as "clockDevice"; "clockNow" names the clock that
    // setting gives ("Patch Prime", the device's label, or Patch Prime while
    // the device is unplugged).
    void SetClockDevice(const QString &key);
    // The mixer's sample rate in Hz (48000 by default) and block size in
    // frames (0 = PipeWire picks). In the state as "sampleRate", "blockSize".
    void SetSampleRate(int rate);
    void SetBlockSize(int frames);
    // Per hardware device: its own sample rate (0 = the mixer's), and its
    // scheduling: "auto", "timer" or "interrupt". Applying these restarts
    // WirePlumber (a few seconds' dropout).
    void SetDeviceSampleRate(const QString &key, int rate);
    void SetDeviceScheduling(const QString &key, const QString &scheduling);
    // The level of an existing send in dB (-60 = silent .. +12).
    void SetSendLevel(const QString &inputKey, const QString &destinationKey, double db);
    // A strip's effects (built-in; in the state per device as "effects", and
    // the types on offer as "effectTypes"): add one of a type at the end,
    // remove or move one by index, bypass it, set a parameter.
    void AddEffect(const QString &key, const QString &type);
    void RemoveEffect(const QString &key, int index);
    void MoveEffect(const QString &key, int from, int to);
    void SetEffectEnabled(const QString &key, int index, bool enabled);
    void SetEffectParam(const QString &key, int index, const QString &name, double value);
    // The strip an effect listens to (a ducker's key): its key, or "" for none.
    void SetEffectKey(const QString &key, int index, const QString &sourceKey);
    // Cue (solo in the headphones): the hardware output it plays to (a key,
    // "" for none; in the state as "cueOutput"), and whether a strip is cued
    // (per device as "cued"). While anything is cued, the cue output plays
    // only the cued strips. Not saved for strips, not part of scenes.
    void SetCueOutput(const QString &key);
    void SetCue(const QString &key, bool cued);
    // kind: "sink" (apps play into it; record its monitor, e.g. "Stream Mix"),
    // "source" (apps record from it like a microphone, e.g. "Chat Mic") or
    // "input" (apps play into it, and it's routed like an input, e.g. "Desktop").
    // Returns the new device's key. Rename with SetLabel, delete with Forget.
    QString CreateVirtual(const QString &name, const QString &kind);
    // Sends an app's streams to a node (a strip's busName, say), or to the
    // system default when nodeName is empty. Remembered for the app.
    void SetAppTarget(const QString &appKey, const QString &nodeName);
    // Forgets an app that isn't running.
    void ForgetApp(const QString &appKey);
    // Scenes: the whole mix (levels, mutes, mono, sends and send levels, app
    // routes, default output and input) saved under a name, and loaded back
    // without creating, deleting or restarting anything. In the state as
    // "scenes" (names) and "currentScene" (the last saved or loaded).
    void SaveScene(const QString &name);
    void LoadScene(const QString &name);
    void DeleteScene(const QString &name);
    void WatchLevels();
    void UnwatchLevels();
    // While a client shows a strip's effects window: its effects' input and
    // output levels ("fxio|..." in Levels), which cost CPU only while watched.
    void WatchEffectLevels(const QString &key);
    void UnwatchEffectLevels(const QString &key);
    // Recording mix buses and routed inputs to files in the recordings folder
    // (state: "recordingFolder", "recordingFormat", "recording" = {key: file},
    // "recordingError"). Start returns an error message, or "" on success.
    QString StartRecording(const QString &key);
    void StopRecording(const QString &key);
    // "flac", "wav" or "mp3" (those this build can write are in the state as
    // "recordingFormats"); applies to the next recording.
    void SetRecordingFormat(const QString &format);
    // The folder's sound files, newest first, as JSON: [{path, name,
    // seconds, bytes, modified (ISO date)}]. "recordingsRevision" in the
    // state goes up when they change.
    QString GetRecordings();
    // The player (state: "player" = {path, paused, duration}, "playerTarget"
    // = the key of the strip it plays into). Play returns an error message,
    // or "" on success.
    QString PlayRecording(const QString &path);
    void PausePlayback(bool paused);
    void StopPlayback();
    void SeekPlayback(double seconds);
    void SetPlayerTarget(const QString &key);

Q_SIGNALS:
    void StateChanged(const QString &state);
    void Levels(const QVariantMap &levels);

private:
    [[nodiscard]] int rowFor(const QString &key);
    [[nodiscard]] QString clockNow() const;
    void scheduleState();
    void updateMetering();
    void updateEffectMetering();
    void emitLevels();

    pp::PipeWireGraph *m_graph;
    pp::DeviceModel *m_devices;
    pp::BusManager *m_buses;
    pp::AppRouter *m_apps;
    pp::RecordingManager *m_recordings;
    int m_recordingsRevision = 0;
    pp::SceneStore *m_scenes;
    QString m_currentScene;
    QTimer m_stateTimer;
    QTimer m_levelsTimer;
    QDBusServiceWatcher m_watcher;
    QSet<QString> m_levelWatchers;
    QHash<QString, QSet<QString>> m_effectLevelWatchers; // caller -> strip keys
};

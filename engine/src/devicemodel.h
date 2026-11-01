// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "endpoint.h"

#include <QAbstractListModel>
#include <QTimer>

#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace pp {

class PipeWireGraph;

// Every audio input and output: the ones PipeWire has now, plus the ones the
// user configured that are currently missing (shown offline, never forgotten
// until the user says so). Rows are sorted inputs first, then hardware before
// virtual, then by system description, so editing a label never moves a row.
//
// When an endpoint appears under a node.name nothing is saved for, it's matched
// against the offline saved endpoints by signature (see matchSignature) and,
// when one clearly fits, that entry is moved to the new name.
class PluginCatalog;

class DeviceModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    // Unscoped on purpose: Qt passes roles around as plain ints.
    enum Role : std::uint16_t // NOLINT(cppcoreguidelines-use-enum-class)
    {
        KeyRole = Qt::UserRole + 1,
        LabelRole,       // the user's label, or the description when there's none
        CustomLabelRole, // the user's label, possibly empty
        DescriptionRole,
        NodeNameRole,
        DirectionRole, // "input" or "output"
        VirtualRole,
        OnlineRole,
        EnabledRole,
        ChannelsRole,
        PositionsRole,    // e.g. "FL FR"
        DefaultRole,      // the system default sink or source
        SavedRole,        // has an entry in the store
        SectionStartRole, // first row of its direction; the QML draws the header
        VolumeRole,       // fader position in dB; minimumVolumeDb() means silent
        MutedRole,
        BusIdRole,
        BusDefaultRole,  // the strip's bus is the system default output or input
        KindRole,        // "hardware", "virtual-sink", "virtual-source" or "virtual-input"
        SendsRole,       // inputs: keys of the outputs this input is sent to
        MonoRole,        // inputs: mixed down to one channel
        BusNameRole,     // node.name of the strip's bus while enabled, else empty
        ChannelPairRole, // hardware: 0 = all channels, n = stereo pair 2n-1/2n
        StageRole,       // "input", "bus" or "output"
        SendLevelsRole,  // {destination key: dB} for this strip's sends (0 dB when missing)
        DeviceRateRole,  // hardware: its own sample rate, 0 = the mixer's
        SchedulingRole,  // hardware: "auto", "timer" or "interrupt"
        EffectsRole,     // [{type, enabled, params: {name: value}}] in order
        CuedRole,        // heard on the cue output now
    };
    Q_ENUM(Role)

    explicit DeviceModel(DeviceStore *store, QObject *parent = nullptr);
    ~DeviceModel() override;
    DeviceModel(const DeviceModel &) = delete;
    DeviceModel &operator=(const DeviceModel &) = delete;
    DeviceModel(DeviceModel &&) = delete;
    DeviceModel &operator=(DeviceModel &&) = delete;

    // Follows a live graph. Without one, feed the model with the node functions
    // below (that's what the tests do).
    void attach(PipeWireGraph *graph);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void setEnabled(int row, bool enabled);
    Q_INVOKABLE void setLabel(int row, const QString &label);
    // Drops the saved settings. An offline endpoint disappears from the list.
    Q_INVOKABLE void forget(int row);
    Q_INVOKABLE void setVolume(int row, double volumeDb);
    Q_INVOKABLE void setMuted(int row, bool muted);
    Q_INVOKABLE void setMono(int row, bool mono);
    // Hardware with more than two channels: use all of them (0) or one stereo
    // pair (1 = channels 1-2, ...). Anything out of range is ignored.
    Q_INVOKABLE void setChannelPair(int row, int pair);
    // Sends (or stops sending) a strip to one in a later stage: an input to a
    // mix bus or an output, a mix bus to an output.
    Q_INVOKABLE void setSend(int row, const QString &destinationKey, bool enabled);
    // The level of an existing send, clamped like the fader.
    Q_INVOKABLE void setSendLevel(int row, const QString &destinationKey, double db);
    // A strip's effects chain (see EffectSettings): add one of a type at the
    // end (with its initial parameters; up to maxEffectsPerStrip), remove,
    // reorder, bypass, and set a parameter (clamped to its range). Unknown
    // types, indexes and parameters are ignored.
    Q_INVOKABLE void addEffect(int row, const QString &type);
    Q_INVOKABLE void removeEffect(int row, int index);
    Q_INVOKABLE void moveEffect(int row, int from, int to);
    Q_INVOKABLE void setEffectEnabled(int row, int index, bool enabled);
    Q_INVOKABLE void setEffectParam(int row, int index, const QString &name, double value);
    // The strip an effect listens to (a ducker's key): another strip's key,
    // or empty for none. Ignored for effects that don't take one.
    Q_INVOKABLE void setEffectKey(int row, int index, const QString &key);
    // A plugin's own state (see EffectSettings::state), as its host saved it.
    void setEffectState(int row, int index, const std::string &state);
    // The installed plugins, for adding and checking plugin effects (types
    // "lv2:<uri>"); without it, only built-in effects can be added.
    void setPluginCatalog(const PluginCatalog *catalog) { m_pluginCatalog = catalog; }
    [[nodiscard]] const PluginCatalog *pluginCatalog() const { return m_pluginCatalog; }
    // Cue: the hardware output it plays to (a key, or empty to turn cue off
    // and clear every cued strip), and whether a strip is cued. Cued strips
    // are temporary: not saved, not part of scenes.
    void setCueOutput(const std::string &key);
    // Recording settings (see DeviceStore); emit recordingSettingsChanged.
    void setRecordingFormat(const std::string &format);
    void setPlayerTarget(const std::string &key);
    // The scene last saved or loaded (see DeviceStore); saved, no signal.
    void setCurrentScene(const std::string &name);
    // The control surface's MIDI port (see DeviceStore); emits
    // controlSurfaceChanged.
    void setControlSurface(const std::string &id);
    // MIDI learn (see DeviceStore): the controller's port and the mappings;
    // emit midiControllerChanged and midiMappingsChanged. Removing or moving
    // an effect moves its mappings with it.
    void setMidiController(const std::string &id);
    void setMidiMappings(std::vector<MidiMapping> mappings);
    // Global hotkeys (see DeviceStore); emits hotkeysChanged. Effects'
    // hotkeys follow them like the mappings.
    void setHotkeys(std::vector<Hotkey> hotkeys);
    // The tray icon and on-screen messages (see DeviceStore); emit
    // desktopSettingsChanged.
    void setTrayIcon(bool on);
    // The soundboard (see DeviceStore); emit soundboardChanged.
    void setPads(std::vector<SoundPad> pads);
    void setSoundboardTarget(const std::string &key);
    void setShowOsd(bool on);
    Q_INVOKABLE void setCue(int row, bool cued);
    [[nodiscard]] const std::set<std::string> &cued() const { return m_cued; }
    // Sets a strip's mix from a scene (see mixFromJson), keeping only what
    // still applies: the level clamped, mono on inputs only, sends to strips
    // that exist in a later stage. False when there's no such strip.
    bool applyMix(const std::string &key, const QJsonObject &mix);
    // What clocks the graph; buses are rebuilt with the new driver priority.
    void setClock(ClockMode clock);
    // Makes a hardware device the clock (switches to the hardware clock).
    void setClockDevice(const std::string &key);
    // The mixer's sample rate and block size (0 = PipeWire picks).
    void setSampleRate(int rate);
    void setBlockSize(int frames);
    // Per hardware device: its own rate (0 = the mixer's) and scheduling.
    void setDeviceSampleRate(int row, int rate);
    void setDeviceScheduling(int row, Scheduling scheduling);
    // Makes a new virtual device, enabled, and returns its key.
    QString createVirtual(const QString &name, EndpointKind kind);

    // The fader's bottom: at or below this the bus is silent.
    static constexpr double minimumVolumeDb() { return -60.0; }
    static constexpr double maximumVolumeDb() { return 12.0; }

    struct Row
    {
        Endpoint endpoint;
        std::optional<std::uint32_t> nodeId; // set while online
    };
    [[nodiscard]] const std::vector<Row> &rows() const { return m_rows; }
    [[nodiscard]] const DeviceStore &store() const { return *m_store; }

    [[nodiscard]] bool isConnected() const { return m_connected; }

    void upsertNode(const GraphObject &node, const GraphObject *device);
    void removeNode(std::uint32_t nodeId);
    void setDefaults(const std::string &sinkName, const std::string &sourceName);
    void setConnected(bool connected);

    [[nodiscard]] int rowForKey(const std::string &key) const;

Q_SIGNALS:
    void connectedChanged();
    // Anything the bus manager acts on changed for this endpoint: enabled,
    // label, volume, mute, forgotten, or its key moved (then both keys).
    void settingsChanged(const QString &key);
    // A saved endpoint was recognised under a new node.name.
    void reattached(const QString &label, const QString &oldNodeName, const QString &newNodeName);
    void clockChanged();
    // The cue output or the set of cued strips changed.
    void cueChanged();
    // The mixer's rate or block size, or a device's rate or scheduling.
    void audioSettingsChanged();
    // The recording format or the player's target.
    void recordingSettingsChanged();
    void controlSurfaceChanged();
    void midiControllerChanged();
    void midiMappingsChanged();
    void hotkeysChanged();
    void desktopSettingsChanged();
    void soundboardChanged();

private:
    const PluginCatalog *m_pluginCatalog = nullptr;
    [[nodiscard]] static bool lessThan(const Row &a, const Row &b);
    [[nodiscard]] int rowForNode(std::uint32_t nodeId) const;
    [[nodiscard]] int findReattachCandidate(const Endpoint &live) const;
    [[nodiscard]] bool isDefault(const Row &row) const;
    [[nodiscard]] Stage stageOfRow(const Row &row) const;
    void insertSorted(Row row);
    void replaceRow(int index, Row row);
    void removeRowAt(int index);
    void refreshSectionStarts();
    void remember(const Row &row, bool seenNow);
    void saveStore();
    void saveLater();
    // Runs edit on a strip's effects (making its entry if needed); when it
    // returns true, saves and announces the change.
    bool editEffects(int row, const std::function<bool(std::vector<EffectSettings> &)> &edit);
    std::set<std::string> m_cued;
    // Effect mappings on a strip after its chain changed: map(old index) is
    // the new index, or -1 for gone.
    void remapEffects(int row, const std::function<int(int)> &map);

    DeviceStore *m_store;
    PipeWireGraph *m_graph = nullptr;
    std::vector<Row> m_rows;
    std::string m_defaultSink;
    std::string m_defaultSource;
    bool m_connected = false;
    QTimer m_saveTimer;
};

} // namespace pp

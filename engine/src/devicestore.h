// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "endpoint.h"
#include "midimapping.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

// Hardware endpoints come from PipeWire; virtual devices are made by Patch
// Prime itself and exist only as long as their saved entry does.
enum class EndpointKind : std::uint8_t
{
    Hardware,      // any endpoint PipeWire reports (cards, other apps' virtual devices)
    VirtualSink,   // apps play into it, others record its monitor ("Stream Mix")
    VirtualSource, // apps record from it, like a microphone ("Chat Mic")
    VirtualInput,  // apps play into it, and it's routed like an input ("Desktop", "Music")
};

// "hardware", "virtual-sink", "virtual-source", "virtual-input" (in the settings
// file and the D-Bus state).
std::string kindName(EndpointKind kind);

// True for the key of a virtual device Patch Prime made (see
// DeviceStore::virtualKey), whatever its node is called.
bool isPatchPrimeVirtual(std::string_view key);
// The reverse; anything unknown is Hardware.
EndpointKind kindFromName(std::string_view name);

// Where a strip sits in the console. Sends only go to a later stage
// (input -> bus, input -> output, bus -> output), so routing can't loop.
enum class Stage : std::uint8_t
{
    Input,  // inputs and virtual inputs: they feed sends
    Bus,    // mix buses (virtual sinks): fed by inputs, they feed outputs
    Output, // hardware outputs and virtual microphones: the end of the line
};

Stage stageOf(Direction direction, EndpointKind kind);

// What clocks the audio graph (the PipeWire "driver").
enum class ClockMode : std::uint8_t
{
    // Patch Prime's own buses: unplugging any interface doesn't stall the
    // rest. Hardware follows with PipeWire's rate matching.
    Internal,
    // A hardware device (DeviceStore::clockDevice(), or PipeWire's pick when
    // none is chosen): the lowest latency, but unplugging that device stalls
    // everything linked to it for a moment.
    Hardware,
};

// How a hardware device's driver is woken (ALSA scheduling).
enum class Scheduling : std::uint8_t
{
    Auto,      // timer while it follows Patch Prime's clock, else the device's default
    Timer,     // timer-based: follows another clock smoothly (rate matching)
    Interrupt, // the device's interrupts: tightest timing when it is the clock
};

// "auto" / "timer" / "interrupt"; anything unknown is Auto.
std::string schedulingName(Scheduling scheduling);
Scheduling schedulingFromName(std::string_view name);

// The mixer's sample rate when nothing has been chosen.
constexpr int defaultSampleRate = 48000;

// The sound card part of an ALSA node name ("usb-Focusrite_Scarlett_8i6_USB_
// F8TFW0D970427A-00" for "alsa_input.usb-Focusrite_…-00.pro-input-0"), so a
// card's input and output can be told apart from other cards'. Empty for
// anything that isn't an ALSA node.
std::string alsaCardOf(std::string_view nodeName);

// "internal" / "hardware"; anything unknown is Internal.
std::string clockModeName(ClockMode mode);
ClockMode clockModeFromName(std::string_view name);
// "input", "bus", "output" (in the D-Bus state).
std::string stageName(Stage stage);

// One effect in a strip's chain: a built-in type (see makeEffect), on or
// bypassed, and its parameters by name (missing ones take their initial value).
struct EffectSettings
{
    std::string type;
    bool enabled = true;
    std::map<std::string, double> params;
    // Effects that listen to another strip (see effectTakesKey): that
    // strip's endpoint key; empty for none.
    std::string key;
    // A plugin's own state apart from its parameters (what its window may
    // have changed: a loaded file, say), as the plugin host saved it.
    std::string state;

    bool operator==(const EffectSettings &) const = default;
};

// What the user chose for one endpoint, kept across restarts and replugs.
struct DeviceSettings
{
    EndpointKind kind = EndpointKind::Hardware;
    // Permanent random id, made when the entry is created. It names the
    // endpoint's bus, so the bus keeps its name when the endpoint is recognised
    // under a new node.name.
    std::string busId;
    // The node.name of this endpoint's bus (for a virtual device, of the
    // device itself), readable in apps that list node names, e.g.
    // "patchprime.out.maudio-1" or "patchprime.desktop". Fixed once given:
    // defaults, app routes and apps' own settings refer to it. Empty in
    // files from before 2026-10-30 (then the old id-based name is used).
    std::string busName;
    bool enabled = false;
    std::string label; // empty: show the system description
    double volumeDb = 0.0;
    bool muted = false;
    // Inputs and mix buses: the keys of the strips this one is sent to.
    std::vector<std::string> sends;
    // The level of each send in dB, by destination key; 0 dB when missing.
    std::map<std::string, double> sendLevels;
    // Inputs only: mix every hardware channel into a one-channel bus, so a mic
    // on one input of a stereo interface is heard in both ears.
    bool mono = false;
    // Hardware with more than two channels (a card in pro-audio mode): 0 uses
    // every channel; n uses only the stereo pair 2n-1 and 2n (1 = channels
    // 1-2, 2 = 3-4, ...), and the bus is stereo.
    int channelPair = 0;
    // The strip's effects, in order, run after its bus. Empty: no processing
    // node at all.
    std::vector<EffectSettings> effects;
    // Hardware: a sample rate for this device (resampled to the mixer's), or
    // 0 to run at the mixer's rate; and how its driver is scheduled.
    int sampleRate = 0;
    Scheduling scheduling = Scheduling::Auto;
    // Last seen details, so an offline endpoint can still be shown and matched.
    std::string nodeName;
    std::string mediaClass;
    Direction direction = Direction::Output;
    bool isVirtual = false;
    std::string description;
    int channels = 0;
    std::vector<std::string> positions;
    Signature signature;
    QDateTime lastSeen;
};

// A strip's mix, as a scene stores it: level, mute, mono, sends, send levels
// and effects. The one place that decides what a scene covers per strip.
QJsonObject mixToJson(const DeviceSettings &settings);
// Sets the mix fields present in json (others stay as they are).
void mixFromJson(DeviceSettings &settings, const QJsonObject &json);
// An effects chain as JSON, and back; effects of unknown types are dropped.
QJsonArray effectsToJson(const std::vector<EffectSettings> &effects);
std::vector<EffectSettings> effectsFromJson(const QJsonArray &json);

// ~/.config/patchprime/devices.json, keyed by Endpoint::key. Saves atomically.
// Only endpoints the user has touched (enabled or labelled) are stored.
class DeviceStore
{
public:
    // An empty path means the standard location under ~/.config/patchprime.
    explicit DeviceStore(QString path = {});

    [[nodiscard]] const QString &path() const { return m_path; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    // Returns false and sets lastError() when the file exists but can't be
    // read. A missing file is an empty store.
    bool load();
    bool save();

    [[nodiscard]] ClockMode clock() const { return m_clock; }
    void setClock(ClockMode clock) { m_clock = clock; }
    // With the hardware clock: the key of the device that is the clock.
    [[nodiscard]] const std::string &clockDevice() const { return m_clockDevice; }
    void setClockDevice(std::string key) { m_clockDevice = std::move(key); }
    // The output that cue (solo in the headphones) plays to: a hardware
    // output's key, or empty for none.
    [[nodiscard]] const std::string &cueOutput() const { return m_cueOutput; }
    void setCueOutput(std::string key) { m_cueOutput = std::move(key); }
    // The mixer's sample rate (Hz) and block size (frames; 0 = PipeWire picks).
    [[nodiscard]] int sampleRate() const { return m_sampleRate; }
    void setSampleRate(int rate) { m_sampleRate = rate; }
    [[nodiscard]] int blockSize() const { return m_blockSize; }
    void setBlockSize(int frames) { m_blockSize = frames; }
    // Recording: the file format ("flac", "wav" or "mp3"), and the strip the
    // player plays into (a key, or empty for none chosen).
    [[nodiscard]] const std::string &recordingFormat() const { return m_recordingFormat; }
    void setRecordingFormat(std::string format) { m_recordingFormat = std::move(format); }
    [[nodiscard]] const std::string &playerTarget() const { return m_playerTarget; }
    void setPlayerTarget(std::string key) { m_playerTarget = std::move(key); }
    // The scene last saved or loaded (empty for none), so it's still known
    // after a restart.
    [[nodiscard]] const std::string &currentScene() const { return m_currentScene; }
    void setCurrentScene(std::string name) { m_currentScene = std::move(name); }
    // The MIDI port of the Mackie Control surface (see AlsaMidi::Port::id),
    // or empty for none.
    [[nodiscard]] const std::string &controlSurface() const { return m_controlSurface; }
    void setControlSurface(std::string id) { m_controlSurface = std::move(id); }
    // The MIDI port of a generic controller (MIDI learn), or empty for none,
    // and what its controls do. Mappings follow their strips when rekeyed,
    // and go with them when removed.
    [[nodiscard]] const std::string &midiController() const { return m_midiController; }
    void setMidiController(std::string id) { m_midiController = std::move(id); }
    [[nodiscard]] const std::vector<MidiMapping> &midiMappings() const { return m_midiMappings; }
    void setMidiMappings(std::vector<MidiMapping> mappings) { m_midiMappings = std::move(mappings); }

    [[nodiscard]] const std::map<std::string, DeviceSettings> &entries() const { return m_entries; }
    [[nodiscard]] const DeviceSettings *find(const std::string &key) const;
    // Creates the entry (with a new busId) when it doesn't exist.
    DeviceSettings &upsert(const std::string &key);
    // Also removes the sends that pointed at it.
    bool remove(const std::string &key);
    // Moves an entry to a new key when its endpoint is recognised under a new
    // name, and updates every send that pointed at the old key.
    bool rekey(const std::string &from, const std::string &to);

    static QString defaultPath();
    static std::string newBusId();
    // Key for a virtual device Patch Prime makes: "patchprime-virtual|<busId>".
    static std::string virtualKey(const std::string &busId);

private:
    QString m_path;
    QString m_lastError;
    ClockMode m_clock = ClockMode::Internal;
    std::string m_clockDevice;
    std::string m_cueOutput;
    int m_sampleRate = defaultSampleRate;
    int m_blockSize = 0;
    std::string m_recordingFormat = "flac";
    std::string m_playerTarget;
    std::string m_currentScene;
    std::string m_controlSurface;
    std::string m_midiController;
    std::vector<MidiMapping> m_midiMappings;
    std::map<std::string, DeviceSettings> m_entries;
};

} // namespace pp

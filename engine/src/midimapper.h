// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "midimapping.h"
#include "midiport.h"

#include <QObject>
#include <QTimer>

#include <map>
#include <optional>
#include <string>

namespace pp {

class DeviceModel;
class RecordingManager;

// Any MIDI controller driving the mixer, through mappings the user teaches
// it (MIDI learn): startLearn(target), then the next control moved on the
// controller does that from now on. Mappings are saved with the devices
// (DeviceStore::midiMappings).
//
// Faders and knobs (CC, pitch bend) set levels and parameters. An absolute
// one takes over only once it reaches the current value (pickup), so
// nothing jumps; a relative one (an endless encoder) moves it up or down.
// Buttons (notes, or CCs crossing 64) toggle mutes, cues, sends, effects and
// recording, or load a scene. The controller is sent the values back (LEDs,
// motorised faders), except for what it just sent itself.
class MidiMapper : public QObject
{
    Q_OBJECT

public:
    MidiMapper(DeviceModel *devices, MidiPort *port, QObject *parent = nullptr);

    // Optional: recording targets do nothing without it.
    void setRecordings(RecordingManager *recordings);

    // Learns the next control moved (replacing what it did before, and
    // what did this target before); cancelLearn stops waiting.
    void startLearn(const MidiTarget &target);
    void cancelLearn();
    [[nodiscard]] const std::optional<MidiTarget> &learning() const { return m_learning; }

    void remove(const std::string &id);
    void setRelative(const std::string &id, bool relative);
    // What a target is, for people: "Mic: level", "Mic → Stream Mix: send".
    [[nodiscard]] std::string describe(const MidiTarget &target) const;

    // A target's position, 0..1 (levels and parameters), or nullopt when
    // it's gone or isn't one.
    [[nodiscard]] std::optional<double> position(const MidiTarget &target) const;
    // Whether a toggle target is on, or nullopt.
    [[nodiscard]] std::optional<bool> isOn(const MidiTarget &target) const;

Q_SIGNALS:
    void learningChanged();
    // A scene control was pressed.
    void sceneRequested(const QString &name);

private:
    // What a mapping last saw and sent (not saved).
    struct Live
    {
        int lastIn = -1;       // the control's last value
        bool pickedUp = false; // an absolute control has reached the value
        double lastSet = -1.0; // the position it last set
        int lastSent = -1;     // the value last sent back
    };

    void handle(const std::vector<std::uint8_t> &message);
    void learn(MidiKind kind, int channel, int number, int value);
    void apply(const MidiMapping &mapping, int value, bool pressed);
    void setPosition(const MidiTarget &target, double position);
    void toggle(const MidiTarget &target);
    [[nodiscard]] bool isContinuous(const MidiTarget &target) const;
    void scheduleFeedback();
    void sendFeedback();

    DeviceModel *m_devices;
    MidiPort *m_port;
    RecordingManager *m_recordings = nullptr;
    MidiParser m_parser;
    std::optional<MidiTarget> m_learning;
    std::map<std::string, Live> m_live; // by mapping id
    QTimer m_feedbackTimer;
};

} // namespace pp

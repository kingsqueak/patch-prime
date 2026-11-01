// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include "midiport.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pp {

class BusManager;
class DeviceModel;
class RecordingManager;

// The fader law: a Mackie fader's 14-bit position (0..16383) as a level in
// dB (DeviceModel::minimumVolumeDb() at the bottom: silent), and back.
// Unity gain sits three quarters of the way up, as on a console.
double mackieFaderDb(int value);
int mackieFaderValue(double db);
// The same law on a 0..1 scale (any fader-like control: a MIDI fader or
// knob's position), unrounded.
double faderLawPosition(double db);
double faderLawDb(double position);
// A level (dB) as a Mackie meter step, 0 (below -60 dB) to 12 (0 dB).
int mackieMeterStep(double db);
// The answer to a Mackie Control's "host connection query" challenge.
std::array<std::uint8_t, 4> mackieChallengeResponse(const std::array<std::uint8_t, 4> &challenge);

// A Mackie Control (or a controller in its Mackie mode: Behringer X-Touch,
// Icon, PreSonus FaderPort 8/16, ...) driving the mixer, over a MidiPort.
//
// Its eight strips show eight of the mixer's strips (inputs, then mix buses,
// then outputs, as the mixer shows them); BANK moves them by eight, CHANNEL
// by one. Per strip: the fader sets the level (motorised faders follow the
// mixer, except while touched), MUTE mutes, SOLO cues (solo in the
// headphones), REC records it (strips that can be recorded), and the V-Pot
// sets its send to the chosen destination (pushing it turns the send on or
// off). SELECT on a mix bus or output makes it that destination; SEND steps
// through them. The master fader is the default output's. The display shows
// each strip's name and level (or send level), the meters its level.
// F1..F8 load scenes 1..8 (see sceneRequested).
class MackieControl : public QObject
{
    Q_OBJECT

public:
    MackieControl(DeviceModel *devices, MidiPort *port, QObject *parent = nullptr);

    // Optional: meters (needs metering on: see wantsMeters) and recording.
    void setBuses(BusManager *buses) { m_buses = buses; }
    void setRecordings(RecordingManager *recordings);

    // Whether the surface would show meters now (a controller is connected).
    [[nodiscard]] bool wantsMeters() const;

    // The strips (keys) the surface can show, in order, and the first one on
    // its strip 1.
    [[nodiscard]] std::vector<std::string> strips() const;
    [[nodiscard]] int bank() const { return m_bank; }
    // Where the V-Pots send to (a mix bus or output's key; empty for none).
    [[nodiscard]] const std::string &sendTarget() const { return m_sendTarget; }

    static constexpr int stripCount = 8;

Q_SIGNALS:
    // F1..F8 pressed: load the scene at this index (0..7) in name order.
    void sceneRequested(int index);

private:
    // What one strip shows now; compared with what was sent, so only changes
    // go out.
    struct StripView
    {
        int fader = -1;
        bool mute = false;
        bool solo = false;
        bool rec = false;
        bool select = false;
        int ring = -1;
        std::string top;
        std::string bottom;
        bool operator==(const StripView &) const = default;
    };

    void connectionChanged();
    void receive(const QByteArray &bytes);
    void handle(const std::vector<std::uint8_t> &message);
    void handleNote(int note, bool pressed);
    void handleFader(int channel, int value);
    void handleVPot(int strip, int value);
    void handleSysex(const std::vector<std::uint8_t> &message);

    // The strip key on surface strip n (empty when none), and the master's.
    [[nodiscard]] std::string keyAt(int strip) const;
    [[nodiscard]] std::string masterKey() const;
    [[nodiscard]] std::vector<std::string> destinations() const;
    [[nodiscard]] bool canSend(const std::string &from, const std::string &to) const;
    [[nodiscard]] StripView viewOf(const std::string &key) const;
    [[nodiscard]] int faderOf(const std::string &key) const;

    void setBank(int bank);
    void flash(const std::string &text);
    void scheduleSync();
    void sync();
    void sendMeters();
    void resetSurface();
    void sendNote(int note, bool on);
    void sendFader(int channel, int value);
    // One of the display's lines (offset 0 or 56), sending only what changed.
    void sendLine(int offset, const std::string &line, std::string &sent);
    void sendRaw(std::vector<std::uint8_t> message);

    DeviceModel *m_devices;
    MidiPort *m_port;
    BusManager *m_buses = nullptr;
    RecordingManager *m_recordings = nullptr;

    MidiParser m_parser;
    int m_bank = 0;
    std::string m_sendTarget;
    std::array<bool, stripCount + 1> m_touched{}; // the master's last
    std::array<std::optional<StripView>, stripCount> m_sent;
    std::optional<int> m_sentMaster;
    std::array<int, stripCount> m_sentMeter{};
    std::string m_sentTop;
    std::string m_sentBottom;
    std::string m_flash; // a message over the top line, for a moment
    QTimer m_syncTimer;
    QTimer m_meterTimer;
    QTimer m_flashTimer;
};

} // namespace pp

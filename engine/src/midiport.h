// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QByteArray>
#include <QObject>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace pp {

// A two-way MIDI connection to one controller: the ALSA sequencer
// (AlsaMidi), or the tests' stand-in.
class MidiPort : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // Whether a controller is connected now (messages sent otherwise are
    // dropped).
    [[nodiscard]] virtual bool isConnected() const = 0;
    // Sends one complete MIDI message (a system exclusive one included).
    virtual void send(std::span<const std::uint8_t> message) = 0;

Q_SIGNALS:
    // MIDI bytes from the controller, in order; a message may be split over
    // calls (long system exclusive ones are).
    void received(const QByteArray &bytes);
    void connectedChanged();
};

// Cuts a MIDI byte stream into whole messages: channel messages (running
// status included) and system exclusive ones; real-time bytes are dropped.
class MidiParser
{
public:
    void reset() { m_incoming.clear(); }
    void feed(const QByteArray &bytes, const std::function<void(const std::vector<std::uint8_t> &)> &message);

private:
    std::vector<std::uint8_t> m_incoming; // a message being received
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QByteArray>
#include <QObject>

#include <cstdint>
#include <span>

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

} // namespace pp

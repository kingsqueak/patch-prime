// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "midiport.h"

#include <memory>
#include <string>
#include <vector>

class QSocketNotifier;

namespace pp {

// A MIDI port on the ALSA sequencer: a client "Patch Prime" with one port
// ("Control Surface", say), connected both ways to the controller chosen with
// setTarget. Controllers are named "<client>:<port>", never by number, and
// the connection is made again whenever the controller (re)appears.
// Main thread only; nothing here blocks.
class AlsaMidi : public MidiPort
{
    Q_OBJECT

public:
    explicit AlsaMidi(const char *portName = "Control Surface", QObject *parent = nullptr);
    ~AlsaMidi() override;
    AlsaMidi(const AlsaMidi &) = delete;
    AlsaMidi &operator=(const AlsaMidi &) = delete;
    AlsaMidi(AlsaMidi &&) = delete;
    AlsaMidi &operator=(AlsaMidi &&) = delete;

    // False when the sequencer couldn't be opened (no snd-seq).
    [[nodiscard]] bool isAvailable() const;
    // Our own client number (-1 when not available), for the tests.
    [[nodiscard]] int clientId() const;

    struct Port
    {
        std::string id;   // "<client name>:<port name>"
        std::string name; // for people: the port's name
    };
    // Ports a controller could be on: other clients' ports that send MIDI,
    // except the system's, Midi Through and Patch Prime's own.
    [[nodiscard]] std::vector<Port> ports() const;

    // The controller to connect to (a Port::id); empty for none.
    void setTarget(const std::string &id);
    [[nodiscard]] const std::string &target() const { return m_target; }

    [[nodiscard]] bool isConnected() const override { return m_connected; }
    void send(std::span<const std::uint8_t> message) override;

Q_SIGNALS:
    // A port came or went (see ports()).
    void portsChanged();

private:
    struct Impl; // the sequencer handle and MIDI codecs (ALSA's types)

    void readEvents();
    void reconnect();
    void disconnectNow();
    void setConnected(bool connected);

    std::unique_ptr<Impl> d;
    std::vector<std::unique_ptr<QSocketNotifier>> m_notifiers;
    std::string m_target;
    bool m_connected = false;
};

} // namespace pp

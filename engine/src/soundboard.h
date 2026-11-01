// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "soundpad.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <map>
#include <memory>
#include <set>
#include <string>

namespace pp {

class BusManager;
class DeviceModel;
class PipeWireGraph;
class Player;

// Pads that play sound files into one strip (the soundboard's target: a
// mix bus, Desktop or an output), saved with the devices
// (DeviceStore::pads). Pressing a pad plays it from the start; pressing it
// again while it plays stops it. Pads can overlap: each playing one is a
// stream of its own (patchprime.pad.<id>, at the file's rate), linked into
// the target by BusManager.
class Soundboard : public QObject
{
    Q_OBJECT

public:
    Soundboard(PipeWireGraph *graph, DeviceModel *devices, BusManager *buses, QObject *parent = nullptr);
    ~Soundboard() override;
    Soundboard(const Soundboard &) = delete;
    Soundboard &operator=(const Soundboard &) = delete;
    Soundboard(Soundboard &&) = delete;
    Soundboard &operator=(Soundboard &&) = delete;

    // A new pad for a sound file (labelled after it); its id, or empty with
    // error set when the file can't be read.
    std::string add(const QString &path, QString *error = nullptr);
    void remove(const std::string &id);
    // A pad's label, level (dB, -60..+12; applies while it plays too) and loop.
    bool update(const std::string &id, const std::string &label, double gainDb, bool loop);
    // Moves a pad to another place in the order.
    void move(int from, int to);

    // Plays the pad, or stops it if it's playing. False with error set when
    // it can't play (no target strip, the file's gone, PipeWire refused).
    bool trigger(const std::string &id, QString *error = nullptr);
    void stop(const std::string &id);
    void stopAll();
    [[nodiscard]] bool isPlaying(const std::string &id) const { return m_playing.contains(id); }
    [[nodiscard]] std::set<std::string> playing() const;
    // Seconds played and the length, per playing pad.
    [[nodiscard]] std::map<std::string, std::pair<double, double>> progress() const;
    [[nodiscard]] const QString &lastError() const { return m_lastError; }

    static constexpr std::size_t maxPlaying = 16;
    static constexpr std::string_view streamPrefix = "patchprime.pad.";

Q_SIGNALS:
    // Pads started or stopped, or the list changed.
    void changed();

private:
    [[nodiscard]] const SoundPad *find(const std::string &id) const;
    bool play(const SoundPad &pad, QString *error);
    void poll();
    void updateStreams();
    [[nodiscard]] static std::string nodeName(const std::string &id);

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    BusManager *m_buses;
    std::map<std::string, std::shared_ptr<Player>> m_playing; // by pad id
    QString m_lastError;
    QTimer m_poll;
};

} // namespace pp

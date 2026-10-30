// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "graphtypes.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pp {

class DeviceModel;
class DeviceStore;
class PipeWireGraph;

// --- Pure helper (unit tested) ----------------------------------------------

// A sound card that came back only partly: the card (a PipeWire device) is
// there, but an enabled device's node on it is missing, while either a
// sibling node runs in the profile the device was saved with, or the card has
// no nodes at all. Seen when two sessions' audio servers raced for a
// hotplugged card: each opened part of it, or the other took all of it.
struct HalfAttached
{
    std::string card;     // PipeWire device.name, e.g. "alsa_card.usb-Focusrite_…-00"
    std::string key;      // the missing endpoint's key
    bool noNodes = false; // the card has no nodes at all (it may just be switched off)
};

std::vector<HalfAttached> findHalfAttached(const std::map<std::uint32_t, GraphObject> &nodes,
                                           const std::map<std::uint32_t, GraphObject> &devices,
                                           const DeviceStore &store);

// A card's active profile from `pactl --format=json list cards` output
// (e.g. "pro-audio", "output:analog-stereo+input:analog-stereo", "off");
// empty when the card isn't listed. The node's own device.profile.name can
// differ ("analog-stereo"), so the card's is what gets re-applied.
std::string activeCardProfile(const QByteArray &pactlJson, const std::string &card);

// --- HotplugRecovery ---------------------------------------------------------

// Re-applies the active profile of a half-attached card (off, then that
// profile), as one would by hand, once it has stayed half-attached for
// settleTime; at most maxAttempts times per card per window, so it can never
// loop. A card whose active profile is "off" was switched off on purpose and
// is left alone (not asked about again for a window).
class HotplugRecovery : public QObject
{
    Q_OBJECT

public:
    HotplugRecovery(PipeWireGraph *graph, DeviceModel *devices, QObject *parent = nullptr);

    static constexpr std::chrono::seconds settleTime{5};
    static constexpr int maxAttempts = 2;
    static constexpr std::chrono::minutes window{5};

Q_SIGNALS:
    // A card was reset to bring a device back; label is the device's.
    void recovering(const QString &label, const QString &card);

private:
    using Clock = std::chrono::steady_clock;

    void check();
    void reset(const std::string &card, const std::string &profile);
    void restore(const std::string &card, const std::string &profile, int triesLeft);
    void runPactl(const QStringList &args, const std::function<void(bool)> &done);

    static constexpr int restoreTries = 3;

    PipeWireGraph *m_graph;
    DeviceModel *m_devices;
    QTimer m_timer;
    std::map<std::string, Clock::time_point> m_seen; // card -> first seen half-attached
    std::map<std::string, std::vector<Clock::time_point>> m_attempts; // card -> recent resets
    std::map<std::string, Clock::time_point> m_leaveAlone; // card -> until (switched off)
    std::map<std::string, std::string> m_switchedOff;      // cards we switched off -> their profile
};

} // namespace pp

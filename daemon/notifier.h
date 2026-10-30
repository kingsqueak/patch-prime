// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

namespace pp {
class DeviceModel;
class HotplugRecovery;
} // namespace pp

// Desktop notifications (org.freedesktop.Notifications) for things that
// happen while nobody watches the window: an enabled device unplugged or
// back, the clock falling back to Patch Prime when the master interface goes,
// and a half-attached card being reset.
//
// Devices also vanish for a few seconds whenever WirePlumber or PipeWire
// restarts (Patch Prime restarts WirePlumber when its device rules change),
// so an unplug is only announced once a device has stayed gone for
// offlineDelay, "back" only follows an announced unplug, and nothing is
// announced while PipeWire is disconnected or just after it (re)connects;
// what was online before then is remembered, so a device that doesn't come
// back is still announced afterwards.
class Notifier : public QObject
{
    Q_OBJECT

public:
    Notifier(pp::DeviceModel *devices, pp::HotplugRecovery *recovery, QObject *parent = nullptr);

    static constexpr int offlineDelayMs = 10000;
    static constexpr int quietAfterConnectMs = 15000;

    static void notify(const QString &summary, const QString &body);

private:
    struct State
    {
        bool online = false;
        // Running while offline and not yet announced; never expires otherwise.
        QDeadlineTimer offlineSince{QDeadlineTimer::Forever};
        bool announcedOffline = false;
    };

    void scan();

    pp::DeviceModel *m_devices;
    QTimer m_timer;
    QDeadlineTimer m_quiet;         // nothing is announced until it expires
    QHash<QString, State> m_states; // enabled hardware devices by key
};

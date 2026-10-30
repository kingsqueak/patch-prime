// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "notifier.h"

#include "devicemodel.h"
#include "hotplugrecovery.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QVariantMap>

Notifier::Notifier(pp::DeviceModel *devices, pp::HotplugRecovery *recovery, QObject *parent)
    : QObject(parent), m_devices(devices), m_quiet(quietAfterConnectMs)
{
    // Polled once a second: the delays below need a clock anyway, and it's cheap.
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &Notifier::scan);
    m_timer.start();
    connect(devices, &pp::DeviceModel::connectedChanged, this, [this] {
        // Every device drops out with PipeWire and comes back with it. What
        // was online before is kept, so a device that hasn't come back once
        // the quiet time is over is still announced.
        m_quiet.setRemainingTime(quietAfterConnectMs);
    });
    connect(recovery, &pp::HotplugRecovery::recovering, this,
            [](const QString &label, const QString & /*card*/) {
                notify(tr("Reconnecting %1").arg(label),
                       tr("It came back only partly after being plugged in, so Patch Prime is "
                          "resetting its sound card."));
            });
}

void Notifier::notify(const QString &summary, const QString &body)
{
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                  QStringLiteral("/org/freedesktop/Notifications"),
                                                  QStringLiteral("org.freedesktop.Notifications"),
                                                  QStringLiteral("Notify"));
    message.setArguments({QStringLiteral("Patch Prime"), 0U, QStringLiteral("audio-card"), summary,
                          body, QStringList{}, QVariantMap{}, -1});
    QDBusConnection::sessionBus().call(message, QDBus::NoBlock);
}

void Notifier::scan()
{
    const bool quiet = !m_devices->isConnected() || !m_quiet.hasExpired();
    const auto &store = m_devices->store();
    QHash<QString, State> next;
    for (int row = 0; row < m_devices->rowCount(); ++row) {
        const auto index = m_devices->index(row);
        if (!m_devices->data(index, pp::DeviceModel::EnabledRole).toBool() ||
            m_devices->data(index, pp::DeviceModel::KindRole).toString() !=
                QLatin1String("hardware")) {
            continue;
        }
        const auto key = m_devices->data(index, pp::DeviceModel::KeyRole).toString();
        const bool online = m_devices->data(index, pp::DeviceModel::OnlineRole).toBool();
        State state = m_states.value(key, State{.online = online});
        if (quiet) {
            next.insert(key, state); // as before the quiet time; nothing announced
            continue;
        }
        const auto label = m_devices->data(index, pp::DeviceModel::LabelRole).toString();
        const bool isMaster =
            store.clock() == pp::ClockMode::Hardware && key.toStdString() == store.clockDevice();
        if (!online && state.online) {
            state.offlineSince.setRemainingTime(offlineDelayMs); // gone: wait and see
        }
        if (!online && !state.announcedOffline && !state.offlineSince.isForever() &&
            state.offlineSince.hasExpired()) {
            notify(tr("%1 unplugged").arg(label),
                   isMaster ? tr("It was the master clock: Patch Prime keeps time until it's back.")
                            : tr("Its strips wait for it; routing resumes when it's back."));
            state.announcedOffline = true;
        }
        if (online) {
            if (state.announcedOffline) {
                notify(tr("%1 is back").arg(label),
                       isMaster ? tr("It's the master clock again.") : QString());
            }
            state.announcedOffline = false;
            state.offlineSince = QDeadlineTimer(QDeadlineTimer::Forever);
        }
        state.online = online;
        next.insert(key, state);
    }
    m_states = std::move(next);
}

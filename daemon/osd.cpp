// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "osd.h"

#include <QDBusConnection>
#include <QDBusMessage>

namespace osd {

void showText(const QString &icon, const QString &text)
{
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.plasmashell"), QStringLiteral("/org/kde/osdService"),
                                                  QStringLiteral("org.kde.osdService"), QStringLiteral("showText"));
    message.setArguments({icon, text});
    message.setAutoStartService(false); // never start Plasma for this
    QDBusConnection::sessionBus().call(message, QDBus::NoBlock);
}

} // namespace osd

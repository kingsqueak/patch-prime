// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "windowservice.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QWindow>

bool WindowService::claimOrActivate()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || bus.registerService(QLatin1String(serviceName))) {
        return true;
    }
    const auto message = QDBusMessage::createMethodCall(QLatin1String(serviceName), QLatin1String(objectPath),
                                                        QStringLiteral("org.kingsqueak.PatchPrime.Window1"),
                                                        QStringLiteral("Activate"));
    bus.call(message, QDBus::Block, 2000);
    return false;
}

void WindowService::Activate()
{
    if (m_window == nullptr) {
        return;
    }
    if (m_window->windowStates().testFlag(Qt::WindowMinimized)) {
        m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
    }
    m_window->show();
    m_window->raise();
    m_window->requestActivate();
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "tray.h"

#include <QApplication>
#include <QDBusConnection>

#include <cstdio>
#include <exception>

int main(int argc, char *argv[])
{
    try {
        QApplication app(argc, argv);
        QApplication::setApplicationName(QStringLiteral("patchprime-tray"));
        QApplication::setApplicationDisplayName(QStringLiteral("Patch Prime"));
        QApplication::setOrganizationDomain(QStringLiteral("kingsqueak.org"));
        QApplication::setDesktopFileName(QStringLiteral("org.kingsqueak.patchprime"));
        QApplication::setQuitOnLastWindowClosed(false);
        // One tray icon: a second start (autostart and the app both) leaves.
        if (!QDBusConnection::sessionBus().registerService(QStringLiteral("org.kingsqueak.PatchPrime.Tray"))) {
            return 0;
        }
        Tray tray;
        return QApplication::exec();
    } catch (const std::exception &e) {
        std::fputs("patchprime-tray: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}

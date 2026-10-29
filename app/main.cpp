// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "daemonclient.h"

#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <qqml.h>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("patchprime"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Patch Prime"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("kingsqueak.org"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.kingsqueak.patchprime"));

    // Use the Plasma style unless the user picked one.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // The engine runs in patchprimed; this window only shows and edits its state.
    DaemonClient devices;
    qmlRegisterSingletonInstance("org.kingsqueak.patchprime.engine", 1, 0, "Devices", &devices);
    qmlRegisterType<StripFilter>("org.kingsqueak.patchprime.engine", 1, 0, "StripFilter");

    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("startPage"), qEnvironmentVariable("PATCHPRIME_PAGE")}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("org.kingsqueak.patchprime", "Main");

    // Developer aid: PATCHPRIME_SCREENSHOT=<file.png> saves the window once the
    // state has arrived, then quits. Works with QT_QPA_PLATFORM=offscreen.
    // PATCHPRIME_PAGE=devices opens the Devices page first.
    if (const auto shot = qEnvironmentVariable("PATCHPRIME_SCREENSHOT"); !shot.isEmpty()) {
        QTimer::singleShot(2000, &app, [&engine, shot] {
            const auto roots = engine.rootObjects();
            auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
            const bool saved = window != nullptr && window->grabWindow().save(shot);
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }

    return QGuiApplication::exec();
}

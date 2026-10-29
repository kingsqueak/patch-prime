// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"
#include "devicestore.h"
#include "pipewiregraph.h"

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

    // Phase 1: the GUI hosts the engine itself. From Phase 2 this moves into
    // patchprimed and the GUI talks to it over D-Bus.
    pp::DeviceStore store;
    if (!store.load()) {
        qWarning("could not read %s: %s; starting with no saved devices", qPrintable(store.path()),
                 qPrintable(store.lastError()));
    }
    pp::PipeWireGraph graph;
    pp::DeviceModel devices(&store);
    devices.attach(&graph);
    graph.start();

    qmlRegisterSingletonInstance("org.kingsqueak.patchprime.engine", 1, 0, "Devices", &devices);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("org.kingsqueak.patchprime", "Main");

    // Developer aid: PATCHPRIME_SCREENSHOT=<file.png> saves the window once the
    // graph has settled, then quits. Works with QT_QPA_PLATFORM=offscreen.
    if (const auto shot = qEnvironmentVariable("PATCHPRIME_SCREENSHOT"); !shot.isEmpty()) {
        QTimer::singleShot(1500, &app, [&engine, shot] {
            const auto roots = engine.rootObjects();
            auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
            const bool saved = window != nullptr && window->grabWindow().save(shot);
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }

    return QGuiApplication::exec();
}

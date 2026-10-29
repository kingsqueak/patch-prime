// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

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

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("org.kingsqueak.patchprime", "Main");

    return QGuiApplication::exec();
}

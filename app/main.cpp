// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "daemonclient.h"
#include "windowservice.h"

#include <QDBusConnection>
#include <QFile>
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
    // Qt adds the display name to every window's title (" — Patch Prime 0.14"),
    // so the version shows in each title bar; windows don't repeat it.
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Patch Prime " PATCHPRIME_VERSION));
    QGuiApplication::setOrganizationDomain(QStringLiteral("kingsqueak.org"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.kingsqueak.patchprime"));
    QGuiApplication::setApplicationVersion(QStringLiteral(PATCHPRIME_VERSION));

    // One window: a second start brings the first forward (screenshots,
    // a developer aid, may run alongside).
    const bool screenshot = qEnvironmentVariableIsSet("PATCHPRIME_SCREENSHOT");
    if (!screenshot && !WindowService::claimOrActivate()) {
        return 0;
    }

    // Use the Plasma style unless the user picked one.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // The engine runs in patchprimed; this window only shows and edits its state.
    DaemonClient devices;
    qmlRegisterSingletonInstance("org.kingsqueak.patchprime.engine", 1, 0, "Devices", &devices);
    qmlRegisterType<StripFilter>("org.kingsqueak.patchprime.engine", 1, 0, "StripFilter");

    // The user guide (docs/HELP.md, built in), for Help > User Guide.
    QFile guide(QStringLiteral(":/docs/HELP.md"));
    const QString guideText =
        guide.open(QIODevice::ReadOnly) ? QString::fromUtf8(guide.readAll()) : QString();

    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("startPage"), qEnvironmentVariable("PATCHPRIME_PAGE")},
         {QStringLiteral("openEffects"), qEnvironmentVariable("PATCHPRIME_EFFECTS")},
         {QStringLiteral("guideText"), guideText}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("org.kingsqueak.patchprime", "Main");

    WindowService windowService;
    if (!screenshot && !engine.rootObjects().isEmpty()) {
        windowService.setWindow(qobject_cast<QWindow *>(engine.rootObjects().first()));
        QDBusConnection::sessionBus().registerObject(QLatin1String(WindowService::objectPath), &windowService,
                                                     QDBusConnection::ExportAllSlots);
    }

    // Developer aid: PATCHPRIME_WINDOW_SIZE=<width>x<height> sizes the window
    // (for screenshots at a given size).
    if (const auto size = qEnvironmentVariable("PATCHPRIME_WINDOW_SIZE").split(QLatin1Char('x'));
        size.size() == 2 && !engine.rootObjects().isEmpty()) {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
            window->resize(size[0].toInt(), size[1].toInt());
        }
    }

    // Developer aid: PATCHPRIME_SCREENSHOT=<file.png> saves the window once the
    // state has arrived, then quits. Works with QT_QPA_PLATFORM=offscreen.
    // PATCHPRIME_PAGE=devices opens the Devices page first (also routing,
    // apps, about, guide: the user guide's window, saved as <file>-2.png);
    // PATCHPRIME_EFFECTS=<strip label> opens that strip's effects;
    // PATCHPRIME_WINDOW_SIZE=<w>x<h> sizes the window (see above).
    if (const auto shot = qEnvironmentVariable("PATCHPRIME_SCREENSHOT"); !shot.isEmpty()) {
        // PATCHPRIME_SCREENSHOT_DELAY=<ms> waits longer (meters settle).
        const int delay = qEnvironmentVariableIsSet("PATCHPRIME_SCREENSHOT_DELAY")
                              ? qEnvironmentVariableIntValue("PATCHPRIME_SCREENSHOT_DELAY")
                              : 2000;
        QTimer::singleShot(delay, &app, [&engine, shot] {
            const auto roots = engine.rootObjects();
            auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
            bool saved = window != nullptr && window->grabWindow().save(shot);
            // Other open windows (an effects window) as <file>-2.png, -3, ...
            int n = 1;
            for (auto *other : QGuiApplication::topLevelWindows()) {
                auto *quick = qobject_cast<QQuickWindow *>(other);
                if (quick != nullptr && quick != window && quick->isVisible()) {
                    auto name = shot;
                    name.insert(name.lastIndexOf(QLatin1Char('.')), QStringLiteral("-%1").arg(++n));
                    saved = quick->grabWindow().save(name) && saved;
                }
            }
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }

    return QGuiApplication::exec();
}

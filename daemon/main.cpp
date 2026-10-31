// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// patchprimed: the Patch Prime engine. Owns the buses and links in PipeWire and
// offers them to the GUI over D-Bus. Meant to run as a systemd user service.

#include "approuter.h"
#include "appstore.h"
#include "audiosetup.h"
#include "busmanager.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "hotplugrecovery.h"
#include "notifier.h"
#include "pipewiregraph.h"
#include "plugins.h"
#include "recordingmanager.h"
#include "scenes.h"
#include "service.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QSocketNotifier>
#include <QTimer>

#include <array>
#include <csignal>
#include <cstdio>
#include <exception>
#include <print>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Written by the signal handler, so it has to be a plain global.
std::array<int, 2> signalPipe{-1, -1}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// SIGTERM/SIGINT: quit through the event loop, so a pending settings save is
// written. Buses and links linger in PipeWire, so audio keeps flowing.
void onSignal(int /*signal*/)
{
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(signalPipe[0], &byte, 1);
}

void installSignalHandlers(QCoreApplication &app)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, signalPipe.data()) != 0) {
        return;
    }
    auto *notifier = new QSocketNotifier(signalPipe[1], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
        char byte = 0;
        [[maybe_unused]] const auto bytesRead = ::read(signalPipe[1], &byte, 1);
        QCoreApplication::quit();
    });
    struct sigaction action{};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

int run(int &argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("patchprimed"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PATCHPRIME_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Patch Prime audio engine."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption removeBuses(
        QStringLiteral("remove-buses"),
        QStringLiteral("Remove every Patch Prime bus from PipeWire and exit."));
    parser.addOption(removeBuses);
    parser.process(app);

    installSignalHandlers(app);

    pp::DeviceStore store;
    if (!store.load()) {
        qWarning("could not read %s: %s; starting with no saved devices", qPrintable(store.path()),
                 qPrintable(store.lastError()));
    }
    pp::AppStore appStore;
    if (!appStore.load()) {
        qWarning("could not read %s: %s; starting with no saved app routes",
                 qPrintable(appStore.path()), qPrintable(appStore.lastError()));
    }

    // Strips from before readable bus names get one now; app routes follow,
    // and the bus manager moves any system default over (setRenamed).
    const auto renamed = pp::migrateBusNames(store);
    if (!renamed.empty()) {
        for (const auto &[from, to] : renamed) {
            std::println("patchprimed: {} is now called {}", from, to);
        }
        store.save();
        for (const auto &[key, route] : appStore.entries()) {
            if (const auto it = renamed.find(route.target); it != renamed.end()) {
                appStore.upsert(key).target = it->second;
            }
        }
        appStore.save();
    }
    pp::PipeWireGraph graph;
    pp::DeviceModel devices(&store);
    devices.attach(&graph);

    if (parser.isSet(removeBuses)) {
        // No BusManager: it would recreate the buses. Wait for the registry to
        // fill, remove, then give the requests a moment to go out.
        QObject::connect(&graph, &pp::PipeWireGraph::connectedChanged, &app, [&graph] {
            QTimer::singleShot(500, [&graph] {
                pp::BusManager::removeAllBusesIn(graph);
                QTimer::singleShot(300, [] { QCoreApplication::quit(); });
            });
        });
        graph.start();
        return QCoreApplication::exec();
    }

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        std::println(stderr, "patchprimed: no D-Bus session bus");
        return 1;
    }
    if (!bus.registerService(QString::fromLatin1(PatchPrimeService::serviceName))) {
        std::println(stderr, "patchprimed: {} is taken; is patchprimed already running?",
                     PatchPrimeService::serviceName);
        return 1;
    }

    pp::PluginCatalog plugins;
    devices.setPluginCatalog(&plugins);
    plugins.start();
    pp::BusManager buses(&graph, &devices);
    buses.setRenamed(renamed);
    pp::AppRouter apps(&graph, &appStore);
    pp::AudioSetup setup(&graph, &devices);
    pp::HotplugRecovery recovery(&graph, &devices);
    Notifier notifier(&devices, &recovery);
    pp::SceneStore scenes;
    pp::RecordingManager recordings(&graph, &devices, &buses);
    PatchPrimeService service(&graph, &devices, &buses, &apps, &scenes, &recordings, &plugins);
    bus.registerObject(QString::fromLatin1(PatchPrimeService::objectPath), &service,
                       QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);

    graph.start();
    const int result = QCoreApplication::exec();
    bus.unregisterService(QString::fromLatin1(PatchPrimeService::serviceName));
    return result;
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::fputs("patchprimed: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}

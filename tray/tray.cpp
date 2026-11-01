// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "tray.h"

#include <KStatusNotifierItem>

#include <QActionGroup>
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QProcess>

namespace {

const auto service = QStringLiteral("org.kingsqueak.PatchPrime");
const auto path = QStringLiteral("/org/kingsqueak/PatchPrime");
const auto interface = QStringLiteral("org.kingsqueak.PatchPrime1");

constexpr int wheelStep = 120; // one notch
constexpr double minimumDb = -60.0;

QString dbText(double db)
{
    return db <= minimumDb ? QStringLiteral("−∞ dB") : QStringLiteral("%1 dB").arg(db, 0, 'f', 1);
}

} // namespace

Tray::Tray(QObject *parent)
    : QObject(parent)
    , m_item(std::make_unique<KStatusNotifierItem>(QStringLiteral("org.kingsqueak.patchprime.tray")))
    , m_menu(new QMenu) // owned by the item once set
{
    m_item->setCategory(KStatusNotifierItem::Hardware);
    m_item->setTitle(QStringLiteral("Patch Prime"));
    m_item->setIconByName(QStringLiteral("audio-input-microphone"));

    m_mute = m_menu->addAction(QIcon::fromTheme(QStringLiteral("microphone-sensitivity-muted")), tr("Mute"));
    m_mute->setCheckable(true);
    connect(m_mute, &QAction::triggered, this, &Tray::toggleMute);
    m_scenes = m_menu->addMenu(QIcon::fromTheme(QStringLiteral("view-media-playlist")), tr("Scenes"));
    m_sceneGroup = new QActionGroup(m_scenes); // NOLINT(cppcoreguidelines-prefer-member-initializer): needs m_scenes
    m_pads = m_menu->addMenu(QIcon::fromTheme(QStringLiteral("media-playlist-play")), tr("Soundboard"));
    m_menu->addSeparator();
    connect(m_menu->addAction(QIcon::fromTheme(QStringLiteral("view-media-equalizer")), tr("Open Patch Prime")),
            &QAction::triggered, this, &Tray::openWindow);
    m_item->setContextMenu(m_menu);

    connect(m_item.get(), &KStatusNotifierItem::activateRequested, this, [] { openWindow(); });
    connect(m_item.get(), &KStatusNotifierItem::secondaryActivateRequested, this, [this] { toggleMute(); });
    connect(m_item.get(), &KStatusNotifierItem::scrollRequested, this, [this](int delta, Qt::Orientation) {
        nudgeLevel(delta / wheelStep);
    });

    auto bus = QDBusConnection::sessionBus();
    bus.connect(service, path, interface, QStringLiteral("StateChanged"), this, SLOT(onStateChanged(QString)));
    auto *watcher = new QDBusServiceWatcher(service, bus,
                                            QDBusServiceWatcher::WatchForRegistration |
                                                QDBusServiceWatcher::WatchForUnregistration,
                                            this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &Tray::fetchState);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { setEngineRunning(false); });
    fetchState();
}

Tray::~Tray() = default;

QJsonObject Tray::micOf(const QJsonObject &state)
{
    QJsonObject fallback;
    for (const auto &d : state.value(QLatin1String("devices")).toArray()) {
        const auto strip = d.toObject();
        if (!strip.value(QLatin1String("isEnabled")).toBool() ||
            strip.value(QLatin1String("stage")).toString() != QLatin1String("input")) {
            continue;
        }
        const bool hardware = strip.value(QLatin1String("kind")).toString() == QLatin1String("hardware");
        if (hardware && strip.value(QLatin1String("busIsDefault")).toBool()) {
            return strip; // what apps record from
        }
        if (hardware && fallback.isEmpty()) {
            fallback = strip;
        }
    }
    return fallback;
}

void Tray::fetchState()
{
    // Not autostarting the engine: the tray only follows it.
    auto message = QDBusMessage::createMethodCall(service, path, interface, QStringLiteral("GetState"));
    message.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            setEngineRunning(false);
        } else {
            onStateChanged(reply.value());
        }
    });
}

void Tray::onStateChanged(const QString &state)
{
    apply(QJsonDocument::fromJson(state.toUtf8()).object());
}

void Tray::apply(const QJsonObject &state)
{
    if (!state.value(QLatin1String("trayIcon")).toBool(true)) {
        QApplication::quit(); // turned off in the settings
        return;
    }
    setEngineRunning(true);
    m_mic = micOf(state);
    const bool haveMic = !m_mic.isEmpty();
    const bool muted = m_mic.value(QLatin1String("muted")).toBool();
    const auto label = m_mic.value(QLatin1String("label")).toString();
    m_item->setIconByName(haveMic && muted ? QStringLiteral("microphone-sensitivity-muted")
                                           : QStringLiteral("audio-input-microphone"));
    QString status = tr("No microphone in the mixer");
    if (haveMic) {
        status = muted ? tr("%1: muted").arg(label)
                       : tr("%1: live, %2").arg(label, dbText(m_mic.value(QLatin1String("volumeDb")).toDouble()));
    }
    m_item->setToolTip(m_item->iconName(), QStringLiteral("Patch Prime"), status);
    m_mute->setEnabled(haveMic);
    m_mute->setText(haveMic ? tr("Mute %1").arg(label) : tr("Mute (no microphone in the mixer)"));
    m_mute->setChecked(muted);

    for (auto *old : m_sceneGroup->actions()) {
        m_sceneGroup->removeAction(old);
    }
    m_scenes->clear(); // deletes the old actions
    const auto scenes = state.value(QLatin1String("scenes")).toArray();
    const auto current = state.value(QLatin1String("currentScene")).toString();
    m_scenes->setEnabled(!scenes.isEmpty());
    for (const auto &s : scenes) {
        const auto name = s.toString();
        auto *action = m_scenes->addAction(name);
        action->setCheckable(true);
        action->setChecked(name == current);
        m_sceneGroup->addAction(action);
        connect(action, &QAction::triggered, this, [name] { call(QStringLiteral("LoadScene"), {name}); });
    }

    // The soundboard: a pad plays (or stops) when picked.
    m_pads->clear();
    const auto pads = state.value(QLatin1String("pads")).toArray();
    m_pads->setEnabled(!pads.isEmpty());
    bool anyPlaying = false;
    for (const auto &p : pads) {
        const auto pad = p.toObject();
        const auto id = pad.value(QLatin1String("id")).toString();
        const bool playing = pad.value(QLatin1String("playing")).toBool();
        anyPlaying = anyPlaying || playing;
        auto *action = m_pads->addAction(
            QIcon::fromTheme(playing ? QStringLiteral("media-playback-stop") : QStringLiteral("media-playback-start")),
            pad.value(QLatin1String("label")).toString());
        connect(action, &QAction::triggered, this, [id] { call(QStringLiteral("TriggerPad"), {id}); });
    }
    if (anyPlaying) {
        m_pads->addSeparator();
        connect(m_pads->addAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")), tr("Stop All")),
                &QAction::triggered, this, [] { call(QStringLiteral("StopAllPads"), {}); });
    }
}

void Tray::setEngineRunning(bool running)
{
    m_running = running;
    m_item->setStatus(running ? KStatusNotifierItem::Active : KStatusNotifierItem::Passive);
    if (!running) {
        m_mic = {};
        m_item->setIconByName(QStringLiteral("audio-input-microphone"));
        m_item->setToolTip(m_item->iconName(), QStringLiteral("Patch Prime"), tr("The engine isn't running"));
        m_mute->setEnabled(false);
        m_scenes->setEnabled(false);
        m_pads->setEnabled(false);
    }
}

void Tray::toggleMute()
{
    if (!m_mic.isEmpty()) {
        call(QStringLiteral("SetMuted"),
             {m_mic.value(QLatin1String("key")).toString(), !m_mic.value(QLatin1String("muted")).toBool()});
    }
}

void Tray::nudgeLevel(int steps)
{
    if (m_mic.isEmpty() || steps == 0) {
        return;
    }
    const double db = m_mic.value(QLatin1String("volumeDb")).toDouble() + steps;
    call(QStringLiteral("SetVolume"), {m_mic.value(QLatin1String("key")).toString(), std::round(db)});
}

void Tray::openWindow()
{
    // A running window comes forward (see the app's WindowService); else
    // the app starts.
    auto bus = QDBusConnection::sessionBus();
    const auto window = QStringLiteral("org.kingsqueak.PatchPrime.Window");
    if (bus.interface() != nullptr && bus.interface()->isServiceRegistered(window).value()) {
        bus.call(QDBusMessage::createMethodCall(window, QStringLiteral("/Window"),
                                                QStringLiteral("org.kingsqueak.PatchPrime.Window1"),
                                                QStringLiteral("Activate")),
                 QDBus::NoBlock);
    } else {
        QProcess::startDetached(QStringLiteral("patchprime"), {});
    }
}

void Tray::call(const QString &method, const QVariantList &args)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(args);
    QDBusConnection::sessionBus().call(message, QDBus::NoBlock);
}

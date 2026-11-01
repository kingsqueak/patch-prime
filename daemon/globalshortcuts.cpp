// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "globalshortcuts.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {

constexpr auto service = "org.kde.kglobalaccel";
constexpr auto path = "/kglobalaccel";
constexpr auto interface = "org.kde.KGlobalAccel";
constexpr auto componentInterface = "org.kde.kglobalaccel.Component";

// kglobalaccel's setShortcut flags.
constexpr uint setPresent = 2;    // the action is there now (it fires)
constexpr uint noAutoloading = 4; // these keys, not the saved ones

QDBusMessage call(const char *method, const QVariantList &args)
{
    auto message = QDBusMessage::createMethodCall(QLatin1String(service), QLatin1String(path),
                                                  QLatin1String(interface), QLatin1String(method));
    message.setArguments(args);
    return message;
}

} // namespace

KGlobalAccelShortcuts::KGlobalAccelShortcuts(QString component, QString friendlyName, QObject *parent)
    : ShortcutBackend(parent)
    , m_component(std::move(component))
    , m_friendlyName(std::move(friendlyName))
{
    auto bus = QDBusConnection::sessionBus();
    // Activatable: it starts when first called.
    const auto *busInterface = bus.interface();
    m_available = busInterface != nullptr &&
                  (busInterface->isServiceRegistered(QLatin1String(service)).value() ||
                   busInterface->activatableServiceNames().value().contains(QLatin1String(service)));

    const auto componentPath = QStringLiteral("/component/") + m_component;
    bus.connect(QLatin1String(service), componentPath, QLatin1String(componentInterface),
                QStringLiteral("globalShortcutPressed"), this,
                SLOT(onPressed(QString, QString, qlonglong)));
    bus.connect(QLatin1String(service), componentPath, QLatin1String(componentInterface),
                QStringLiteral("globalShortcutRepeated"), this,
                SLOT(onRepeated(QString, QString, qlonglong)));
    bus.connect(QLatin1String(service), componentPath, QLatin1String(componentInterface),
                QStringLiteral("globalShortcutReleased"), this,
                SLOT(onReleased(QString, QString, qlonglong)));
    bus.connect(QLatin1String(service), QLatin1String(path), QLatin1String(interface),
                QStringLiteral("yourShortcutGotChanged"), this,
                SLOT(onKeysChanged(QStringList, QList<int>)));

    // kglobalaccel restarted (with KWin, say): everything has to be
    // registered again.
    m_watcher.setConnection(bus);
    m_watcher.addWatchedService(QLatin1String(service));
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForRegistration);
    connect(&m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
        m_available = true;
        Q_EMIT availableChanged();
    });
}

KGlobalAccelShortcuts::~KGlobalAccelShortcuts()
{
    for (const auto &[id, name] : m_names) {
        QDBusConnection::sessionBus().call(call("setInactive", {actionId(id)}), QDBus::NoBlock);
    }
}

QStringList KGlobalAccelShortcuts::actionId(const std::string &id) const
{
    const auto at = m_names.find(id);
    return {m_component, QString::fromStdString(id), m_friendlyName,
            at == m_names.end() ? QString::fromStdString(id) : QString::fromStdString(at->second)};
}

void KGlobalAccelShortcuts::add(const std::string &id, const std::string &name)
{
    m_names[id] = name;
    QDBusConnection::sessionBus().call(call("doRegister", {actionId(id)}), QDBus::NoBlock);
    // No keys, autoloading: kglobalaccel answers with the saved ones.
    setShortcut(id, {}, setPresent);
}

void KGlobalAccelShortcuts::remove(const std::string &id)
{
    QDBusConnection::sessionBus().call(
        call("unregister", {m_component, QString::fromStdString(id)}), QDBus::NoBlock);
    m_names.erase(id);
    m_keys.erase(id);
}

void KGlobalAccelShortcuts::setKeys(const std::string &id, const QList<int> &keys)
{
    if (m_names.contains(id)) {
        setShortcut(id, keys, setPresent | noAutoloading);
    }
}

void KGlobalAccelShortcuts::setShortcut(const std::string &id, const QList<int> &keys, uint flags)
{
    const auto pending = QDBusConnection::sessionBus().asyncCall(
        call("setShortcut", {actionId(id), QVariant::fromValue(keys), flags}));
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, id](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QList<int>> reply = *w;
        if (reply.isError() || !m_names.contains(id)) {
            return;
        }
        auto saved = reply.value();
        saved.removeAll(0);
        m_keys[id] = saved;
        Q_EMIT keysChanged(QString::fromStdString(id));
    });
}

QList<int> KGlobalAccelShortcuts::keys(const std::string &id) const
{
    const auto at = m_keys.find(id);
    return at == m_keys.end() ? QList<int>{} : at->second;
}

void KGlobalAccelShortcuts::forgetComponent()
{
    for (const auto &[id, name] : m_names) {
        QDBusConnection::sessionBus().call(call("unregister", {m_component, QString::fromStdString(id)}));
    }
    m_names.clear();
    m_keys.clear();
    QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(
        QLatin1String(service), QStringLiteral("/component/") + m_component,
        QLatin1String(componentInterface), QStringLiteral("cleanUp")));
}

void KGlobalAccelShortcuts::onPressed(const QString &component, const QString &action, qlonglong /*timestamp*/)
{
    if (component == m_component) {
        Q_EMIT pressed(action);
    }
}

void KGlobalAccelShortcuts::onRepeated(const QString &component, const QString &action, qlonglong /*timestamp*/)
{
    if (component == m_component) {
        Q_EMIT repeated(action);
    }
}

void KGlobalAccelShortcuts::onReleased(const QString &component, const QString &action, qlonglong /*timestamp*/)
{
    if (component == m_component) {
        Q_EMIT released(action);
    }
}

void KGlobalAccelShortcuts::onKeysChanged(const QStringList &actionId, const QList<int> &keys)
{
    if (actionId.size() < 2 || actionId.at(0) != m_component) {
        return;
    }
    const auto id = actionId.at(1).toStdString();
    if (!m_names.contains(id)) {
        return;
    }
    auto cleaned = keys;
    cleaned.removeAll(0);
    m_keys[id] = cleaned;
    Q_EMIT keysChanged(actionId.at(1));
}

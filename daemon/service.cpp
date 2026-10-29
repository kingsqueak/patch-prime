// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "service.h"

#include "approuter.h"
#include "busmanager.h"
#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

PatchPrimeService::PatchPrimeService(pp::PipeWireGraph *graph, pp::DeviceModel *devices,
                                     pp::BusManager *buses, pp::AppRouter *apps,
                                     QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices), m_buses(buses), m_apps(apps)
{
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(30);
    connect(&m_stateTimer, &QTimer::timeout, this, [this] { Q_EMIT StateChanged(GetState()); });

    connect(devices, &QAbstractItemModel::dataChanged, this, &PatchPrimeService::scheduleState);
    connect(devices, &QAbstractItemModel::rowsInserted, this, &PatchPrimeService::scheduleState);
    connect(devices, &QAbstractItemModel::rowsRemoved, this, &PatchPrimeService::scheduleState);
    connect(devices, &QAbstractItemModel::modelReset, this, &PatchPrimeService::scheduleState);
    connect(devices, &pp::DeviceModel::connectedChanged, this, &PatchPrimeService::scheduleState);
    connect(apps, &pp::AppRouter::changed, this, &PatchPrimeService::scheduleState);

    m_levelsTimer.setInterval(33);
    connect(&m_levelsTimer, &QTimer::timeout, this, &PatchPrimeService::emitLevels);

    // Forget level watchers that leave without saying so (closed, crashed).
    m_watcher.setConnection(QDBusConnection::sessionBus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this](const QString &service) {
                m_watcher.removeWatchedService(service);
                m_levelWatchers.remove(service);
                updateMetering();
            });
}

QString PatchPrimeService::GetState() const
{
    const auto roles = m_devices->roleNames();
    QJsonArray devices;
    for (int row = 0; row < m_devices->rowCount(); ++row) {
        const auto index = m_devices->index(row);
        QJsonObject device;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            device.insert(QString::fromUtf8(it.value()),
                          QJsonValue::fromVariant(m_devices->data(index, it.key())));
        }
        devices.append(device);
    }
    QJsonArray apps;
    for (const auto &app : m_apps->apps()) {
        apps.append(QJsonObject{
            {QStringLiteral("key"), QString::fromStdString(app.key)},
            {QStringLiteral("name"), QString::fromStdString(app.settings.name)},
            {QStringLiteral("icon"), QString::fromStdString(app.settings.icon)},
            {QStringLiteral("direction"),
             QString::fromStdString(pp::streamDirectionName(app.settings.direction))},
            {QStringLiteral("target"), QString::fromStdString(app.settings.target)},
            {QStringLiteral("streams"), app.streams},
        });
    }
    const QJsonObject state{
        {QStringLiteral("connected"), m_devices->isConnected()},
        {QStringLiteral("devices"), devices},
        {QStringLiteral("apps"), apps},
    };
    return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

int PatchPrimeService::rowFor(const QString &key)
{
    const int row = m_devices->rowForKey(key.toStdString());
    if (row < 0 && calledFromDBus()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no device with key ") + key);
    }
    return row;
}

void PatchPrimeService::SetEnabled(const QString &key, bool enabled)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setEnabled(row, enabled);
    }
}

void PatchPrimeService::SetLabel(const QString &key, const QString &label)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setLabel(row, label);
    }
}

void PatchPrimeService::SetVolume(const QString &key, double volumeDb)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setVolume(row, volumeDb);
    }
}

void PatchPrimeService::SetMuted(const QString &key, bool muted)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setMuted(row, muted);
    }
}

void PatchPrimeService::Forget(const QString &key)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->forget(row);
    }
}

void PatchPrimeService::SetDefaultOutput(const QString &key, bool throughMixer)
{
    const int row = rowFor(key);
    if (row < 0) {
        return;
    }
    const auto &endpoint = m_devices->rows()[static_cast<std::size_t>(row)].endpoint;
    const auto *settings = m_devices->store().find(endpoint.key);
    auto fail = [this](const QString &why) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::Failed, why);
        }
    };
    if (!pp::playsInto(endpoint)) {
        fail(QStringLiteral("not something apps play into: ") + key);
        return;
    }
    if (!throughMixer && settings != nullptr && settings->kind != pp::EndpointKind::Hardware) {
        fail(QStringLiteral("a virtual device has no device to bypass to; make another strip the default"));
        return;
    }
    if (throughMixer && (settings == nullptr || !settings->enabled)) {
        fail(QStringLiteral("enable the device first: ") + key);
        return;
    }
    const auto name = throughMixer ? pp::busNameFor(endpoint, *settings) : endpoint.nodeName;
    if (!m_graph->setConfiguredDefault(pp::Direction::Output, name)) {
        fail(QStringLiteral("PipeWire refused the default device change"));
    }
}

void PatchPrimeService::SetMono(const QString &key, bool mono)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setMono(row, mono);
    }
}

void PatchPrimeService::SetSend(const QString &inputKey, const QString &destinationKey,
                                bool enabled)
{
    if (const int row = rowFor(inputKey); row >= 0) {
        if (m_devices->rowForKey(destinationKey.toStdString()) < 0) {
            if (calledFromDBus()) {
                sendErrorReply(QDBusError::InvalidArgs,
                               QStringLiteral("no device with key ") + destinationKey);
            }
            return;
        }
        m_devices->setSend(row, destinationKey, enabled);
    }
}

QString PatchPrimeService::CreateVirtual(const QString &name, const QString &kind)
{
    const auto trimmed = name.trimmed();
    // "sink" / "source" / "input" here, "virtual-sink" etc. in the state.
    const auto type = pp::kindFromName("virtual-" + kind.toStdString());
    if (trimmed.isEmpty() || type == pp::EndpointKind::Hardware) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("need a name and a kind of \"sink\", \"source\" or \"input\""));
        }
        return {};
    }
    return m_devices->createVirtual(trimmed, type);
}

void PatchPrimeService::SetAppTarget(const QString &appKey, const QString &nodeName)
{
    if (!m_apps->setTarget(appKey.toStdString(), nodeName.toStdString()) && calledFromDBus()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no app with key ") + appKey);
    }
}

void PatchPrimeService::ForgetApp(const QString &appKey)
{
    if (!m_apps->forget(appKey.toStdString()) && calledFromDBus()) {
        sendErrorReply(QDBusError::InvalidArgs,
                       QStringLiteral("no remembered app that isn't running: ") + appKey);
    }
}

void PatchPrimeService::WatchLevels()
{
    if (!calledFromDBus()) {
        return;
    }
    const auto caller = message().service();
    if (!m_levelWatchers.contains(caller)) {
        m_levelWatchers.insert(caller);
        m_watcher.addWatchedService(caller);
        updateMetering();
    }
}

void PatchPrimeService::UnwatchLevels()
{
    if (!calledFromDBus()) {
        return;
    }
    const auto caller = message().service();
    m_watcher.removeWatchedService(caller);
    m_levelWatchers.remove(caller);
    updateMetering();
}

void PatchPrimeService::scheduleState()
{
    if (!m_stateTimer.isActive()) {
        m_stateTimer.start();
    }
}

void PatchPrimeService::updateMetering()
{
    const bool watched = !m_levelWatchers.isEmpty();
    m_buses->setMetering(watched);
    if (watched && !m_levelsTimer.isActive()) {
        m_levelsTimer.start();
    } else if (!watched) {
        m_levelsTimer.stop();
    }
}

void PatchPrimeService::emitLevels()
{
    QVariantMap levels;
    for (const auto &[key, peaks] : m_buses->takeLevels()) {
        QList<double> values;
        values.reserve(static_cast<qsizetype>(peaks.size()));
        for (const float peak : peaks) {
            values.append(static_cast<double>(peak));
        }
        levels.insert(QString::fromStdString(key), QVariant::fromValue(values));
    }
    if (!levels.isEmpty()) {
        Q_EMIT Levels(levels);
    }
}

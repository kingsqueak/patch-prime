// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "daemonclient.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

const QString service = QStringLiteral("org.kingsqueak.PatchPrime");
const QString path = QStringLiteral("/org/kingsqueak/PatchPrime");
const QString interface = QStringLiteral("org.kingsqueak.PatchPrime1");

// Field names in the daemon's state JSON (DeviceModel's role names).
constexpr std::array fields{
    "key",       "label",        "customLabel", "description", "nodeName",    "direction",
    "isVirtual", "online",       "isEnabled",   "channels",    "positions",   "isDefault",
    "saved",     "sectionStart", "volumeDb",    "muted",       "busId",       "busIsDefault",
    "kind",      "sends",        "mono",        "busName",     "channelPair", "stage",
};

// How far a meter falls per update (about 30 per second): 60 dB in ~1.6 s.
constexpr double meterFallPerUpdate = 0.02;

} // namespace

DaemonClient::DaemonClient(QObject *parent) : QAbstractListModel(parent)
{
    int role = Qt::UserRole + 1;
    for (const char *field : fields) {
        m_roles.insert(role, QByteArray(field));
        m_roleIds.insert(QByteArray(field), role);
        ++role;
    }
    m_levelsRole = role;
    m_roles.insert(m_levelsRole, QByteArrayLiteral("levels"));

    auto bus = QDBusConnection::sessionBus();
    bus.connect(service, path, interface, QStringLiteral("StateChanged"), this,
                SLOT(onStateChanged(QString)));
    bus.connect(service, path, interface, QStringLiteral("Levels"), this,
                SLOT(onLevels(QVariantMap)));

    m_watcher.setConnection(bus);
    m_watcher.addWatchedService(service);
    connect(&m_watcher, &QDBusServiceWatcher::serviceRegistered, this,
            [this] { setDaemonRunning(true); });
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this] { setDaemonRunning(false); });

    if (bus.interface()->isServiceRegistered(service)) {
        setDaemonRunning(true);
    } else {
        startDaemon();
    }
}

DaemonClient::~DaemonClient()
{
    if (m_daemonRunning) {
        // Stop the daemon's meter streams now rather than when it notices we left.
        QDBusConnection::sessionBus().call(
            QDBusMessage::createMethodCall(service, path, interface,
                                           QStringLiteral("UnwatchLevels")),
            QDBus::NoBlock);
    }
}

int DaemonClient::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant DaemonClient::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const auto &row = m_rows.at(index.row());
    if (role == m_levelsRole) {
        const auto levels = m_levels.value(row.value(QStringLiteral("key")).toString());
        QVariantList out;
        for (const double level : levels) {
            out.append(level);
        }
        return out;
    }
    if (role == Qt::DisplayRole) {
        role = m_roleIds.value("label");
    }
    return row.value(QString::fromUtf8(m_roles.value(role)));
}

QHash<int, QByteArray> DaemonClient::roleNames() const
{
    return m_roles;
}

void DaemonClient::setDaemonRunning(bool running)
{
    if (running == m_daemonRunning) {
        return;
    }
    m_daemonRunning = running;
    Q_EMIT daemonRunningChanged();
    if (running) {
        setLastError({});
        fetchState();
        call(QStringLiteral("WatchLevels"), {});
    } else {
        beginResetModel();
        m_rows.clear();
        m_levels.clear();
        endResetModel();
        if (m_connected) {
            m_connected = false;
            Q_EMIT connectedChanged();
        }
    }
}

void DaemonClient::startDaemon()
{
    auto *bus = QDBusConnection::sessionBus().interface();
    auto *watcher = new QDBusPendingCallWatcher(
        bus->asyncCall(QStringLiteral("StartServiceByName"), service, 0U), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto *w) {
        w->deleteLater();
        if (!w->isError()) {
            return; // registration arrives through the watcher
        }
        const auto dir = QCoreApplication::applicationDirPath();
        for (const auto &candidate : {dir + QStringLiteral("/patchprimed"),
                                      dir + QStringLiteral("/../daemon/patchprimed")}) {
            if (QFileInfo(candidate).isExecutable() &&
                QProcess::startDetached(QFileInfo(candidate).canonicalFilePath(), {})) {
                return;
            }
        }
        setLastError(
            tr("Can't start the Patch Prime engine (patchprimed): %1").arg(w->error().message()));
    });
}

void DaemonClient::fetchState()
{
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(
            QDBusMessage::createMethodCall(service, path, interface, QStringLiteral("GetState"))),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            setLastError(reply.error().message());
            return;
        }
        onStateChanged(reply.value());
    });
}

void DaemonClient::call(const QString &method, const QVariantList &args)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(args);
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto *w) {
        w->deleteLater();
        if (w->isError()) {
            setLastError(w->error().message());
        }
    });
}

void DaemonClient::setEnabled(const QString &key, bool enabled)
{
    call(QStringLiteral("SetEnabled"), {key, enabled});
}

void DaemonClient::setLabel(const QString &key, const QString &label)
{
    call(QStringLiteral("SetLabel"), {key, label});
}

void DaemonClient::setVolume(const QString &key, double volumeDb)
{
    call(QStringLiteral("SetVolume"), {key, volumeDb});
}

void DaemonClient::setMuted(const QString &key, bool muted)
{
    call(QStringLiteral("SetMuted"), {key, muted});
}

void DaemonClient::forget(const QString &key)
{
    call(QStringLiteral("Forget"), {key});
}

void DaemonClient::setDefaultOutput(const QString &key, bool throughMixer)
{
    call(QStringLiteral("SetDefaultOutput"), {key, throughMixer});
}

void DaemonClient::setMono(const QString &key, bool mono)
{
    call(QStringLiteral("SetMono"), {key, mono});
}

void DaemonClient::setChannelPair(const QString &key, int pair)
{
    call(QStringLiteral("SetChannelPair"), {key, pair});
}

void DaemonClient::setSend(const QString &inputKey, const QString &destinationKey, bool enabled)
{
    call(QStringLiteral("SetSend"), {inputKey, destinationKey, enabled});
}

void DaemonClient::createVirtual(const QString &name, const QString &kind)
{
    call(QStringLiteral("CreateVirtual"), {name, kind});
}

QString DaemonClient::labelFor(const QString &key) const
{
    for (const auto &row : m_rows) {
        if (row.value(QStringLiteral("key")).toString() == key) {
            return row.value(QStringLiteral("label")).toString();
        }
    }
    return {};
}

void DaemonClient::setAppTarget(const QString &appKey, const QString &nodeName)
{
    call(QStringLiteral("SetAppTarget"), {appKey, nodeName});
}

void DaemonClient::forgetApp(const QString &appKey)
{
    call(QStringLiteral("ForgetApp"), {appKey});
}

void DaemonClient::onStateChanged(const QString &state)
{
    const auto root = QJsonDocument::fromJson(state.toUtf8()).object();
    if (const bool connected = root.value(QLatin1String("connected")).toBool();
        connected != m_connected) {
        m_connected = connected;
        Q_EMIT connectedChanged();
    }

    QList<QVariantMap> rows;
    for (const auto &device : root.value(QLatin1String("devices")).toArray()) {
        rows.append(device.toObject().toVariantMap());
    }

    // Targets for the app matrix: every enabled strip's bus. Apps play into
    // outputs, virtual sinks and virtual inputs; they can record from any bus
    // (a sink's is its monitor).
    QVariantList playback;
    QVariantList record;
    for (const auto &row : std::as_const(rows)) {
        const auto busName = row.value(QStringLiteral("busName")).toString();
        if (busName.isEmpty()) {
            continue;
        }
        const auto kind = row.value(QStringLiteral("kind")).toString();
        const QVariantMap target{
            {QStringLiteral("name"), busName},
            {QStringLiteral("label"), row.value(QStringLiteral("label"))},
            {QStringLiteral("kind"), kind},
        };
        record.append(target);
        if ((row.value(QStringLiteral("direction")).toString() == QLatin1String("output") &&
             kind != QLatin1String("virtual-source")) ||
            kind == QLatin1String("virtual-input")) {
            playback.append(target);
        }
    }
    QVariantList apps;
    for (const auto &app : root.value(QLatin1String("apps")).toArray()) {
        apps.append(app.toObject().toVariantMap());
    }
    if (apps != m_apps || playback != m_playbackTargets || record != m_recordTargets) {
        m_apps = std::move(apps);
        m_playbackTargets = std::move(playback);
        m_recordTargets = std::move(record);
        Q_EMIT appsChanged();
    }

    const auto keysOf = [](const QList<QVariantMap> &list) {
        QStringList keys;
        for (const auto &r : list) {
            keys.append(r.value(QStringLiteral("key")).toString());
        }
        return keys;
    };
    if (keysOf(rows) == keysOf(m_rows)) {
        m_rows = std::move(rows);
        if (!m_rows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(rowCount() - 1));
        }
        return;
    }
    // Rows came, went or moved: a reset is simplest and the list is short.
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

double DaemonClient::meterFraction(double peak)
{
    if (peak <= 0.0) {
        return 0.0;
    }
    const double db = 20.0 * std::log10(peak);
    return std::clamp((db - minimumVolumeDb()) / -minimumVolumeDb(), 0.0, 1.0);
}

void DaemonClient::onLevels(const QVariantMap &levels)
{
    for (auto it = levels.begin(); it != levels.end(); ++it) {
        QList<double> peaks;
        const auto &value = it.value();
        if (value.canConvert<QDBusArgument>()) {
            peaks = qdbus_cast<QList<double>>(value.value<QDBusArgument>());
        } else {
            peaks = value.value<QList<double>>();
        }

        auto &shown = m_levels[it.key()];
        shown.resize(peaks.size());
        for (qsizetype c = 0; c < peaks.size(); ++c) {
            shown[c] = std::max(meterFraction(peaks[c]), shown[c] - meterFallPerUpdate);
        }

        const auto row = std::ranges::find_if(m_rows, [&](const QVariantMap &r) {
            return r.value(QStringLiteral("key")).toString() == it.key();
        });
        if (row != m_rows.end()) {
            const auto i = index(static_cast<int>(row - m_rows.begin()));
            Q_EMIT dataChanged(i, i, {m_levelsRole});
        }
    }
}

void DaemonClient::setLastError(const QString &error)
{
    if (error != m_lastError) {
        m_lastError = error;
        Q_EMIT lastErrorChanged();
    }
}

StripFilter::StripFilter(QObject *parent) : QSortFilterProxyModel(parent)
{
    connect(this, &QAbstractItemModel::rowsInserted, this, &StripFilter::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &StripFilter::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &StripFilter::countChanged);
    setDynamicSortFilter(true);
    sort(0);
}

bool StripFilter::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const auto stageRole = sourceModel()->roleNames().key("stage");
    const auto rank = [stageRole](const QModelIndex &index) {
        static const QStringList order{QStringLiteral("input"), QStringLiteral("bus"),
                                       QStringLiteral("output")};
        return order.indexOf(index.data(stageRole).toString());
    };
    const auto a = rank(left);
    const auto b = rank(right);
    return a != b ? a < b : left.row() < right.row();
}

void StripFilter::setStages(const QStringList &stages)
{
    if (stages != m_stages) {
        beginFilterChange();
        m_stages = stages;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
        Q_EMIT stagesChanged();
    }
}

bool StripFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const auto *model = sourceModel();
    if (model == nullptr) {
        return false;
    }
    const auto roles = model->roleNames();
    const auto index = model->index(sourceRow, 0, sourceParent);
    return model->data(index, roles.key("isEnabled")).toBool() &&
           m_stages.contains(model->data(index, roles.key("stage")).toString());
}

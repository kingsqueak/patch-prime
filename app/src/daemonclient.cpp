// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "daemonclient.h"

#include "eq.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QFile>
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
    "key",        "label",        "customLabel", "description", "nodeName",    "direction",
    "isVirtual",  "online",       "isEnabled",   "channels",    "positions",   "isDefault",
    "saved",      "sectionStart", "volumeDb",    "muted",       "busId",       "busIsDefault",
    "kind",       "sends",        "mono",        "busName",     "channelPair", "stage",
    "sendLevels", "deviceRate",   "scheduling",  "effects",     "cued",
};

// How far a meter falls per update (about 30 per second): 60 dB in ~1.6 s.
constexpr double meterFallPerUpdate = 0.02;
// A peak's hold mark stays put for about 1.5 s.
constexpr int peakHoldUpdates = 45;
// Effect meters (dB) fall about 20 dB a second.
constexpr double effectMeterFallPerUpdate = 0.66;
// "Signal present" lights for anything above -100 dBFS (far below the meter's
// range, so a path turned down too far still shows it works), and stays lit
// for about half a second.
constexpr double signalThreshold = 1e-5;
constexpr int signalHoldUpdates = 15;

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
    m_signalRole = role + 1;
    m_roles.insert(m_signalRole, QByteArrayLiteral("signal"));
    // Per channel: the held peak and the RMS (meter fractions); and the
    // short-term loudness in LUFS.
    m_holdRole = role + 2;
    m_roles.insert(m_holdRole, QByteArrayLiteral("peakHold"));
    m_rmsRole = role + 3;
    m_roles.insert(m_rmsRole, QByteArrayLiteral("rms"));
    m_lufsRole = role + 4;
    m_roles.insert(m_lufsRole, QByteArrayLiteral("lufs"));

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
    if (role == m_signalRole) {
        return m_signalHold.value(row.value(QStringLiteral("key")).toString()) > 0;
    }
    if (role == m_holdRole) {
        QVariantList out;
        for (const auto &hold : m_peakHold.value(row.value(QStringLiteral("key")).toString())) {
            out.append(hold.level);
        }
        return out;
    }
    if (role == m_rmsRole) {
        QVariantList out;
        for (const double level : m_rms.value(row.value(QStringLiteral("key")).toString())) {
            out.append(level);
        }
        return out;
    }
    if (role == m_lufsRole) {
        return m_lufs.value(row.value(QStringLiteral("key")).toString(), -70.0);
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
    m_pluginsRevision = -1; // a new daemon counts afresh: fetch its list
    if (running) {
        setLastError({});
        fetchState();
        call(QStringLiteral("WatchLevels"), {});
        for (auto it = m_effectLevelWatches.cbegin(); it != m_effectLevelWatches.cend(); ++it) {
            call(QStringLiteral("WatchEffectLevels"), {it.key()});
        }
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

// The analyzer doesn't follow Qt parents: the watcher isn't leaked.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void DaemonClient::call(const QString &method, const QVariantList &args)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(args);
    // Owned by this (its Qt parent) and deleted when finished.
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto *w) {
        w->deleteLater();
        if (w->isError()) {
            setLastError(w->error().message());
        }
    });
}

void DaemonClient::callReportingError(const QString &method, const QVariantList &args)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(args);
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            setLastError(reply.error().message());
        } else if (!reply.value().isEmpty()) {
            setLastError(reply.value());
        }
    });
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

void DaemonClient::startRecording(const QString &key)
{
    callReportingError(QStringLiteral("StartRecording"), {key});
}

void DaemonClient::stopRecording(const QString &key)
{
    call(QStringLiteral("StopRecording"), {key});
}

void DaemonClient::setRecordingFormat(const QString &format)
{
    call(QStringLiteral("SetRecordingFormat"), {format});
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): QML calls it on the instance
QVariantList DaemonClient::recordings() const
{
    const auto message = QDBusMessage::createMethodCall(service, path, interface,
                                                        QStringLiteral("GetRecordings"));
    const QDBusReply<QString> reply = QDBusConnection::sessionBus().call(message);
    if (!reply.isValid()) {
        return {};
    }
    return QJsonDocument::fromJson(reply.value().toUtf8()).array().toVariantList();
}

void DaemonClient::playRecording(const QString &file)
{
    callReportingError(QStringLiteral("PlayRecording"), {file});
}

void DaemonClient::pausePlayback(bool paused)
{
    call(QStringLiteral("PausePlayback"), {paused});
}

void DaemonClient::stopPlayback()
{
    call(QStringLiteral("StopPlayback"), {});
}

void DaemonClient::seekPlayback(double seconds)
{
    call(QStringLiteral("SeekPlayback"), {seconds});
}

void DaemonClient::setPlayerTarget(const QString &key)
{
    call(QStringLiteral("SetPlayerTarget"), {key});
}

void DaemonClient::setPlaybackLoop(bool loop)
{
    call(QStringLiteral("SetPlaybackLoop"), {loop});
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): QML calls it on the instance
bool DaemonClient::trashFile(const QString &file)
{
    return QFile::moveToTrash(file);
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

void DaemonClient::setDefaultInput(const QString &key, bool throughMixer)
{
    call(QStringLiteral("SetDefaultInput"), {key, throughMixer});
}

void DaemonClient::setMono(const QString &key, bool mono)
{
    call(QStringLiteral("SetMono"), {key, mono});
}

void DaemonClient::setSendLevel(const QString &inputKey, const QString &destinationKey, double db)
{
    call(QStringLiteral("SetSendLevel"), {inputKey, destinationKey, db});
}

void DaemonClient::setCueOutput(const QString &key)
{
    call(QStringLiteral("SetCueOutput"), {key});
}

void DaemonClient::setControlSurface(const QString &id)
{
    call(QStringLiteral("SetControlSurface"), {id});
}

void DaemonClient::setCue(const QString &key, bool cued)
{
    call(QStringLiteral("SetCue"), {key, cued});
}

QVariantMap DaemonClient::effectInfo(const QString &type) const
{
    if (const auto at = m_pluginIndex.find(type); at != m_pluginIndex.end()) {
        auto info = m_plugins.at(*at).toMap();
        info.insert(QStringLiteral("label"), info.value(QStringLiteral("name")));
        info.insert(QStringLiteral("plugin"), true);
        return info;
    }
    for (const auto &t : m_effectTypes) {
        if (const auto info = t.toMap(); info.value(QStringLiteral("type")).toString() == type) {
            return info;
        }
    }
    // A plugin that isn't installed (any more): its URI's last part as a name.
    const bool plugin = type.startsWith(QLatin1String("lv2:")) || type.startsWith(QLatin1String("clap:")) ||
                        type.startsWith(QLatin1String("vst3:"));
    return {{QStringLiteral("type"), type},
            {QStringLiteral("label"), plugin ? type.section(QLatin1Char('/'), -1) : type},
            {QStringLiteral("params"), QVariantList{}},
            {QStringLiteral("plugin"), plugin},
            {QStringLiteral("missing"), true}};
}

void DaemonClient::setPluginWindow(const QString &key, int index, bool open)
{
    call(QStringLiteral("SetPluginWindow"), {key, index, open});
}

void DaemonClient::rescanPlugins()
{
    call(QStringLiteral("RescanPlugins"), {});
}

void DaemonClient::addEffect(const QString &key, const QString &type)
{
    call(QStringLiteral("AddEffect"), {key, type});
}

void DaemonClient::removeEffect(const QString &key, int index)
{
    call(QStringLiteral("RemoveEffect"), {key, index});
}

void DaemonClient::moveEffect(const QString &key, int from, int to)
{
    call(QStringLiteral("MoveEffect"), {key, from, to});
}

void DaemonClient::setEffectEnabled(const QString &key, int index, bool enabled)
{
    call(QStringLiteral("SetEffectEnabled"), {key, index, enabled});
}

void DaemonClient::setEffectParam(const QString &key, int index, const QString &name,
                                  double value)
{
    call(QStringLiteral("SetEffectParam"), {key, index, name, value});
}

QVariantList DaemonClient::eqCurve(const QVariantMap &params, const QVariantList &freqs,
                                  int band) const
{
    const auto value = [&params](const QString &name, double fallback) {
        const auto it = params.find(name);
        return it != params.end() ? it->toDouble() : fallback;
    };
    std::vector<pp::EqEffect::Band> bands;
    for (int b = 1; b <= pp::EqEffect::bandCount; ++b) {
        if (band != 0 && b != band) {
            continue;
        }
        auto d = pp::EqEffect::defaultBand(b - 1);
        const auto prefix = QStringLiteral("b%1").arg(b);
        d.on = value(prefix + QLatin1String("On"), d.on ? 1.0 : 0.0) >= 0.5;
        d.type = static_cast<pp::EqEffect::BandType>(std::clamp(
            static_cast<int>(std::lround(value(prefix + QLatin1String("Type"),
                                               static_cast<double>(d.type)))),
            0, pp::EqEffect::bandTypeCount - 1));
        d.freq = value(prefix + QLatin1String("Freq"), d.freq);
        d.gainDb = value(prefix + QLatin1String("Gain"), d.gainDb);
        d.q = value(prefix + QLatin1String("Q"), d.q);
        if (band != 0) {
            d.on = true; // a single band's own curve, even while it's off
        }
        bands.push_back(d);
    }
    const double rate = m_sampleRate > 0 ? m_sampleRate : 48000.0;
    QVariantList out;
    out.reserve(freqs.size());
    for (const auto &f : freqs) {
        out.append(pp::EqEffect::responseDb(bands, f.toDouble(), rate));
    }
    return out;
}

void DaemonClient::setEffectKey(const QString &key, int index, const QString &sourceKey)
{
    call(QStringLiteral("SetEffectKey"), {key, index, sourceKey});
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

void DaemonClient::setClock(const QString &mode)
{
    call(QStringLiteral("SetClock"), {mode});
}

void DaemonClient::saveScene(const QString &name)
{
    call(QStringLiteral("SaveScene"), {name});
}

void DaemonClient::loadScene(const QString &name)
{
    call(QStringLiteral("LoadScene"), {name});
}

void DaemonClient::deleteScene(const QString &name)
{
    call(QStringLiteral("DeleteScene"), {name});
}

void DaemonClient::setClockDevice(const QString &key)
{
    call(QStringLiteral("SetClockDevice"), {key});
}

void DaemonClient::setSampleRate(int rate)
{
    call(QStringLiteral("SetSampleRate"), {rate});
}

void DaemonClient::setBlockSize(int frames)
{
    call(QStringLiteral("SetBlockSize"), {frames});
}

void DaemonClient::setDeviceSampleRate(const QString &key, int rate)
{
    call(QStringLiteral("SetDeviceSampleRate"), {key, rate});
}

void DaemonClient::setDeviceScheduling(const QString &key, const QString &scheduling)
{
    call(QStringLiteral("SetDeviceScheduling"), {key, scheduling});
}

void DaemonClient::onStateChanged(const QString &state)
{
    const auto root = QJsonDocument::fromJson(state.toUtf8()).object();
    if (const bool connected = root.value(QLatin1String("connected")).toBool();
        connected != m_connected) {
        m_connected = connected;
        Q_EMIT connectedChanged();
    }

    QStringList scenes;
    for (const auto &scene : root.value(QLatin1String("scenes")).toArray()) {
        scenes.append(scene.toString());
    }
    if (const auto current = root.value(QLatin1String("currentScene")).toString();
        scenes != m_scenes || current != m_currentScene) {
        m_scenes = scenes;
        m_currentScene = current;
        Q_EMIT scenesChanged();
    }

    {
        const auto folder = root.value(QLatin1String("recordingFolder")).toString();
        const auto format = root.value(QLatin1String("recordingFormat")).toString();
        const auto formats = root.value(QLatin1String("recordingFormats")).toVariant().toStringList();
        const auto recording = root.value(QLatin1String("recording")).toObject().toVariantMap();
        const auto error = root.value(QLatin1String("recordingError")).toString();
        const int revision = root.value(QLatin1String("recordingsRevision")).toInt();
        const auto player = root.value(QLatin1String("player")).toObject().toVariantMap();
        const auto target = root.value(QLatin1String("playerTarget")).toString();
        if (folder != m_recordingFolder || format != m_recordingFormat ||
            formats != m_recordingFormats ||
            recording != m_recording || error != m_recordingError ||
            revision != m_recordingsRevision || player != m_player || target != m_playerTarget) {
            m_recordingFolder = folder;
            m_recordingFormat = format;
            m_recordingFormats = formats;
            m_recording = recording;
            m_recordingError = error;
            m_recordingsRevision = revision;
            m_player = player;
            m_playerTarget = target;
            // Times of strips that stopped recording, and a stopped player's place, go.
            for (auto it = m_recordingSeconds.begin(); it != m_recordingSeconds.end();) {
                it = m_recording.contains(it.key()) ? std::next(it) : m_recordingSeconds.erase(it);
            }
            if (m_player.value(QStringLiteral("path")).toString().isEmpty()) {
                m_playerPosition = 0.0;
            }
            Q_EMIT recordingChanged();
            Q_EMIT recordingTimesChanged();
        }
    }

    if (const auto cue = root.value(QLatin1String("cueOutput")).toString(); cue != m_cueOutput) {
        m_cueOutput = cue;
        Q_EMIT cueOutputChanged();
    }

    if (const QVariantMap surface{
            {QStringLiteral("id"), root.value(QLatin1String("controlSurface")).toString()},
            {QStringLiteral("available"), root.value(QLatin1String("controlSurfaceAvailable")).toBool()},
            {QStringLiteral("connected"), root.value(QLatin1String("controlSurfaceConnected")).toBool()},
            {QStringLiteral("ports"), root.value(QLatin1String("controlSurfacePorts")).toArray().toVariantList()},
        };
        surface != m_controlSurface) {
        m_controlSurface = surface;
        Q_EMIT controlSurfaceChanged();
    }

    if (const auto types = root.value(QLatin1String("effectTypes")).toArray().toVariantList();
        types != m_effectTypes) {
        m_effectTypes = types;
        Q_EMIT effectTypesChanged();
    }

    {
        const int revision = root.value(QLatin1String("pluginsRevision")).toInt();
        const bool scanning = root.value(QLatin1String("pluginsScanning")).toBool();
        const auto scan = root.value(QLatin1String("pluginsScan")).toObject().toVariantMap();
        const auto lastScan = QDateTime::fromString(root.value(QLatin1String("pluginsLastScan")).toString(),
                                                    Qt::ISODate);
        const auto errors = root.value(QLatin1String("pluginErrors")).toObject().toVariantMap();
        const auto windows = root.value(QLatin1String("pluginWindows")).toObject().toVariantMap();
        const auto windowErrors =
            root.value(QLatin1String("pluginWindowErrors")).toObject().toVariantMap();
        if (revision != m_pluginsRevision || scanning != m_pluginsScanning ||
            errors != m_pluginErrors || windows != m_pluginWindows ||
            windowErrors != m_pluginWindowErrors || scan != m_pluginsScan ||
            lastScan != m_pluginsLastScan) {
            if (revision != m_pluginsRevision) {
                // The list is long (every plugin's parameters): only fetched
                // when it changed.
                const auto message = QDBusMessage::createMethodCall(service, path, interface,
                                                                    QStringLiteral("GetPlugins"));
                const QDBusReply<QString> reply = QDBusConnection::sessionBus().call(message);
                m_plugins = reply.isValid()
                                ? QJsonDocument::fromJson(reply.value().toUtf8()).array().toVariantList()
                                : QVariantList{};
                m_pluginIndex.clear();
                for (qsizetype i = 0; i < m_plugins.size(); ++i) {
                    m_pluginIndex.insert(m_plugins.at(i).toMap().value(QStringLiteral("type")).toString(),
                                         static_cast<int>(i));
                }
                m_pluginsRevision = revision;
            }
            m_pluginsScanning = scanning;
            m_pluginsScan = scan;
            m_pluginsLastScan = lastScan;
            m_pluginErrors = errors;
            m_pluginWindows = windows;
            m_pluginWindowErrors = windowErrors;
            Q_EMIT pluginsChanged();
        }
    }

    QList<QVariantMap> rows;
    for (const auto &device : root.value(QLatin1String("devices")).toArray()) {
        rows.append(device.toObject().toVariantMap());
    }

    QVariantList choices{QVariantMap{{QStringLiteral("value"), QStringLiteral("internal")},
                                     {QStringLiteral("text"), tr("Patch Prime")}}};
    for (const auto &row : std::as_const(rows)) {
        // Only ALSA interfaces can be the clock (Bluetooth and the like can't).
        if (row.value(QStringLiteral("kind")).toString() == QLatin1String("hardware") &&
            row.value(QStringLiteral("isEnabled")).toBool() &&
            row.value(QStringLiteral("nodeName")).toString().startsWith(QLatin1String("alsa_"))) {
            choices.append(
                QVariantMap{{QStringLiteral("value"), row.value(QStringLiteral("key"))},
                            {QStringLiteral("text"), row.value(QStringLiteral("label"))}});
        }
    }
    const auto clock = root.value(QLatin1String("clock")).toString();
    const auto clockDevice = root.value(QLatin1String("clockDevice")).toString();
    const auto clockNow = root.value(QLatin1String("clockNow")).toString();
    const auto rate = root.value(QLatin1String("sampleRate")).toInt();
    const auto block = root.value(QLatin1String("blockSize")).toInt();
    if (clock != m_clock || clockDevice != m_clockDevice || clockNow != m_clockNow ||
        choices != m_clockChoices || rate != m_sampleRate || block != m_blockSize) {
        m_clock = clock;
        m_clockDevice = clockDevice;
        m_clockNow = clockNow;
        m_clockChoices = std::move(choices);
        m_sampleRate = rate;
        m_blockSize = block;
        Q_EMIT clockChanged();
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

double DaemonClient::effectMeterFraction(double peak)
{
    if (peak <= 0.0) {
        return 0.0;
    }
    const double db = 20.0 * std::log10(peak);
    return std::clamp((db - effectMeterFloorDb()) / -effectMeterFloorDb(), 0.0, 1.0);
}

void DaemonClient::onLevels(const QVariantMap &levels)
{
    bool metersChanged = false;
    bool timesChanged = false;
    for (auto it = levels.begin(); it != levels.end(); ++it) {
        QList<double> peaks;
        const auto &value = it.value();
        if (value.canConvert<QDBusArgument>()) {
            peaks = qdbus_cast<QList<double>>(value.value<QDBusArgument>());
        } else {
            peaks = value.value<QList<double>>();
        }

        // Recording and playing times.
        if (it.key().startsWith(QLatin1String("rec|"))) {
            if (!peaks.isEmpty()) {
                m_recordingSeconds.insert(it.key().mid(4), peaks.front());
                timesChanged = true;
            }
            continue;
        }
        if (it.key() == QLatin1String("player")) {
            if (!peaks.isEmpty() && peaks.front() != m_playerPosition) {
                m_playerPosition = peaks.front();
                timesChanged = true;
            }
            continue;
        }

        // RMS and loudness: shown as they are.
        if (it.key().startsWith(QLatin1String("rms|"))) {
            auto &shown = m_rms[it.key().mid(4)];
            shown.clear();
            for (const double r : peaks) {
                shown.append(meterFraction(r));
            }
            continue;
        }
        if (it.key().startsWith(QLatin1String("lufs|"))) {
            if (!peaks.isEmpty()) {
                m_lufs[it.key().mid(5)] = peaks.front();
            }
            continue;
        }

        // An effect's input and output: peaks fall like the strip meters'.
        if (it.key().startsWith(QLatin1String("fxio|"))) {
            const auto at = it.key().mid(5);
            const auto groups = peaks.size() / 4;
            const auto old = m_effectLevels.value(at).toMap();
            QVariantMap shown;
            const std::array<QString, 6> names{QStringLiteral("in"),     QStringLiteral("inRms"),
                                               QStringLiteral("out"),    QStringLiteral("outRms"),
                                               QStringLiteral("key"),    QStringLiteral("keyRms")};
            // Falling as fast in dB as the strip meters.
            const double fall = meterFallPerUpdate * minimumVolumeDb() / effectMeterFloorDb();
            for (qsizetype part = 0; part < 6; ++part) {
                // The key's two values follow the four groups' parts.
                const qsizetype count = part < 4 ? groups : 1;
                const qsizetype first = part < 4 ? part * groups : (4 * groups) + (part - 4);
                if (first + count > peaks.size()) {
                    break;
                }
                const auto &name = names.at(static_cast<std::size_t>(part));
                const auto before = old.value(name).toList();
                const bool isPeak = part % 2 == 0;
                QVariantList values;
                for (qsizetype g = 0; g < count; ++g) {
                    const double now = effectMeterFraction(peaks[first + g]);
                    const double last = g < before.size() ? before[g].toDouble() : 0.0;
                    values.append(isPeak ? std::max(now, last - fall) : now);
                }
                shown.insert(name, values);
            }
            if (shown != old) {
                m_effectLevels.insert(at, shown);
                metersChanged = true;
            }
            continue;
        }

        // An effect's meters: held, falling at about 20 dB a second.
        if (it.key().startsWith(QLatin1String("fx|"))) {
            const auto at = it.key().mid(3);
            const auto old = m_effectMeters.value(at).toList();
            QVariantList shown;
            for (qsizetype i = 0; i < peaks.size(); ++i) {
                const double before = i < old.size() ? old[i].toDouble() : 0.0;
                shown.append(std::max(peaks[i], before - effectMeterFallPerUpdate));
            }
            if (shown != old) {
                m_effectMeters.insert(at, shown);
                metersChanged = true;
            }
            continue;
        }

        auto &shown = m_levels[it.key()];
        shown.resize(peaks.size());
        auto &holds = m_peakHold[it.key()];
        holds.resize(peaks.size());
        for (qsizetype c = 0; c < peaks.size(); ++c) {
            const double now = meterFraction(peaks[c]);
            shown[c] = std::max(now, shown[c] - meterFallPerUpdate);
            // The highest peak stays marked for a while, then falls slowly.
            auto &hold = holds[c];
            if (now >= hold.level) {
                hold = {.level = now, .updates = peakHoldUpdates};
            } else if (hold.updates > 0) {
                --hold.updates;
            } else {
                hold.level = std::max(now, hold.level - (meterFallPerUpdate / 2));
            }
        }
        auto &hold = m_signalHold[it.key()];
        const bool wasLit = hold > 0;
        hold = std::ranges::any_of(peaks, [](double p) { return p > signalThreshold; })
                   ? signalHoldUpdates
                   : std::max(hold - 1, 0);

        const auto row = std::ranges::find_if(m_rows, [&](const QVariantMap &r) {
            return r.value(QStringLiteral("key")).toString() == it.key();
        });
        if (row != m_rows.end()) {
            const auto i = index(static_cast<int>(row - m_rows.begin()));
            if (wasLit != (hold > 0)) {
                Q_EMIT dataChanged(i, i, {m_levelsRole, m_signalRole, m_holdRole, m_rmsRole, m_lufsRole});
            } else {
                Q_EMIT dataChanged(i, i, {m_levelsRole, m_holdRole, m_rmsRole, m_lufsRole});
            }
        }
    }
    if (metersChanged) {
        Q_EMIT effectMetersChanged();
    }
    if (timesChanged) {
        Q_EMIT recordingTimesChanged();
    }
}

void DaemonClient::watchEffectLevels(const QString &key, bool watch)
{
    int &count = m_effectLevelWatches[key];
    count += watch ? 1 : -1;
    if (count <= 0) {
        m_effectLevelWatches.remove(key);
        m_effectLevels.removeIf([&key](QVariantMap::iterator it) { return it.key().startsWith(key + u'|'); });
        Q_EMIT effectMetersChanged();
        if (m_daemonRunning) {
            call(QStringLiteral("UnwatchEffectLevels"), {key});
        }
    } else if (watch && count == 1 && m_daemonRunning) {
        call(QStringLiteral("WatchEffectLevels"), {key});
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

QVariantMap StripFilter::get(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= rowCount()) {
        return out;
    }
    const auto at = index(row, 0);
    const auto roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it) {
        out.insert(QString::fromUtf8(it.value()), data(at, it.key()));
    }
    return out;
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

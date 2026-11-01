// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "service.h"

#include "alsamidi.h"
#include "approuter.h"
#include "busmanager.h"
#include "effects.h"
#include "devicemodel.h"
#include "endpoint.h"
#include "mackiecontrol.h"
#include "pipewiregraph.h"
#include "plugins.h"
#include "recordingmanager.h"
#include "scenes.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <set>

PatchPrimeService::PatchPrimeService(pp::PipeWireGraph *graph, pp::DeviceModel *devices,
                                     pp::BusManager *buses, pp::AppRouter *apps,
                                     pp::SceneStore *scenes, pp::RecordingManager *recordings,
                                     pp::PluginCatalog *plugins, QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices), m_buses(buses), m_apps(apps),
      m_recordings(recordings), m_plugins(plugins), m_scenes(scenes)
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
    connect(devices, &pp::DeviceModel::clockChanged, this, &PatchPrimeService::scheduleState);
    connect(devices, &pp::DeviceModel::cueChanged, this, &PatchPrimeService::scheduleState);
    connect(devices, &pp::DeviceModel::audioSettingsChanged, this,
            &PatchPrimeService::scheduleState);

    connect(recordings, &pp::RecordingManager::changed, this, &PatchPrimeService::scheduleState);
    connect(plugins, &pp::PluginCatalog::changed, this, &PatchPrimeService::scheduleState);
    connect(buses, &pp::BusManager::pluginsChanged, this, &PatchPrimeService::scheduleState);
    connect(recordings, &pp::RecordingManager::filesChanged, this, [this] {
        ++m_recordingsRevision;
        scheduleState();
    });

    m_levelsTimer.setInterval(33);
    connect(&m_levelsTimer, &QTimer::timeout, this, &PatchPrimeService::emitLevels);

    // Forget level watchers that leave without saying so (closed, crashed).
    m_watcher.setConnection(QDBusConnection::sessionBus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this](const QString &service) {
                m_watcher.removeWatchedService(service);
                m_levelWatchers.remove(service);
                m_effectLevelWatchers.remove(service);
                updateMetering();
                updateEffectMetering();
            });
}

void PatchPrimeService::setControlSurface(pp::AlsaMidi *midi, pp::MackieControl *mackie)
{
    m_midi = midi;
    m_mackie = mackie;
    connect(midi, &pp::AlsaMidi::portsChanged, this, &PatchPrimeService::scheduleState);
    connect(midi, &pp::MidiPort::connectedChanged, this, [this] {
        updateMetering();
        scheduleState();
    });
    connect(m_devices, &pp::DeviceModel::controlSurfaceChanged, this,
            &PatchPrimeService::scheduleState);
    connect(mackie, &pp::MackieControl::sceneRequested, this, [this](int index) {
        const auto names = m_scenes->names();
        if (index >= 0 && index < names.size()) {
            LoadScene(names.at(index));
        }
    });
    updateMetering();
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
    QJsonArray effectTypes;
    for (const auto &type : pp::effectTypes()) {
        QJsonArray params;
        for (const auto &p : pp::effectParams(type)) {
            params.append(QJsonObject{
                {QStringLiteral("name"), QString::fromStdString(p.name)},
                {QStringLiteral("label"), QString::fromStdString(p.label)},
                {QStringLiteral("unit"), QString::fromStdString(p.unit)},
                {QStringLiteral("minimum"), p.minimum},
                {QStringLiteral("maximum"), p.maximum},
                {QStringLiteral("initial"), p.initial},
                {QStringLiteral("kind"), QString::fromStdString(pp::paramKindName(p.kind))},
                {QStringLiteral("options"), [&p] {
                     QJsonArray options;
                     for (const auto &o : p.options) {
                         options.append(QString::fromStdString(o));
                     }
                     return options;
                 }()},
            });
        }
        effectTypes.append(QJsonObject{
            {QStringLiteral("takesKey"), pp::effectTakesKey(type)},
            {QStringLiteral("type"), QString::fromStdString(type)},
            {QStringLiteral("label"), QString::fromStdString(pp::effectLabel(type))},
            {QStringLiteral("params"), params},
        });
    }
    const QJsonObject state{
        {QStringLiteral("effectTypes"), effectTypes},
        {QStringLiteral("pluginsRevision"), m_plugins->revision()},
        {QStringLiteral("pluginsScanning"), m_plugins->isScanning()},
        {QStringLiteral("pluginsScan"), QJsonObject{
             {QStringLiteral("done"), m_plugins->progress().done},
             {QStringLiteral("total"), m_plugins->progress().total},
             {QStringLiteral("found"), m_plugins->progress().found},
         }},
        {QStringLiteral("pluginsLastScan"), m_plugins->lastScan().isValid()
                                                ? m_plugins->lastScan().toString(Qt::ISODate)
                                                : QString()},
        {QStringLiteral("pluginWindows"), [this] {
             QJsonObject windows;
             for (const auto &[key, index] : m_buses->pluginWindows()) {
                 auto list = windows.value(QString::fromStdString(key)).toArray();
                 list.append(static_cast<int>(index));
                 windows.insert(QString::fromStdString(key), list);
             }
             return windows;
         }()},
        {QStringLiteral("pluginWindowErrors"), [this] {
             QJsonObject errors;
             for (const auto &[at, message] : m_buses->pluginWindowErrors()) {
                 const auto key = QString::fromStdString(at.first);
                 auto forStrip = errors.value(key).toObject();
                 forStrip.insert(QString::number(at.second), QString::fromStdString(message));
                 errors.insert(key, forStrip);
             }
             return errors;
         }()},
        {QStringLiteral("pluginErrors"), [this] {
             QJsonObject errors;
             for (const auto &[at, message] : m_buses->pluginErrors()) {
                 const auto key = QString::fromStdString(at.first);
                 auto forStrip = errors.value(key).toObject();
                 forStrip.insert(QString::number(at.second), QString::fromStdString(message));
                 errors.insert(key, forStrip);
             }
             return errors;
         }()},
        {QStringLiteral("connected"), m_devices->isConnected()},
        {QStringLiteral("devices"), devices},
        {QStringLiteral("apps"), apps},
        {QStringLiteral("clock"),
         QString::fromStdString(pp::clockModeName(m_devices->store().clock()))},
        {QStringLiteral("clockDevice"), QString::fromStdString(m_devices->store().clockDevice())},
        {QStringLiteral("cueOutput"), QString::fromStdString(m_devices->store().cueOutput())},
        {QStringLiteral("clockNow"), clockNow()},
        {QStringLiteral("sampleRate"), m_devices->store().sampleRate()},
        {QStringLiteral("scenes"), QJsonArray::fromStringList(m_scenes->names())},
        {QStringLiteral("currentScene"), QString::fromStdString(m_devices->store().currentScene())},
        {QStringLiteral("blockSize"), m_devices->store().blockSize()},
        {QStringLiteral("recordingFolder"), m_recordings->folder()},
        {QStringLiteral("recordingFormat"),
         QString::fromStdString(pp::recordingFormatName(m_recordings->format()))},
        {QStringLiteral("recording"), [this] {
             QJsonObject recording;
             for (const auto &[key, path] : m_recordings->recording()) {
                 recording.insert(QString::fromStdString(key), path);
             }
             return recording;
         }()},
        {QStringLiteral("recordingFormats"), [] {
             QJsonArray formats;
             for (const auto f : {pp::RecordingFormat::Flac, pp::RecordingFormat::Wav,
                                  pp::RecordingFormat::Mp3}) {
                 if (pp::recordingFormatAvailable(f)) {
                     formats.append(QString::fromStdString(pp::recordingFormatName(f)));
                 }
             }
             return formats;
         }()},
        {QStringLiteral("recordingError"), m_recordings->lastError()},
        {QStringLiteral("recordingsRevision"), m_recordingsRevision},
        {QStringLiteral("controlSurface"), QString::fromStdString(m_devices->store().controlSurface())},
        {QStringLiteral("controlSurfaceAvailable"), m_midi != nullptr && m_midi->isAvailable()},
        {QStringLiteral("controlSurfaceConnected"), m_midi != nullptr && m_midi->isConnected()},
        {QStringLiteral("controlSurfacePorts"), [this] {
             QJsonArray ports;
             if (m_midi != nullptr) {
                 for (const auto &port : m_midi->ports()) {
                     ports.append(QJsonObject{{QStringLiteral("id"), QString::fromStdString(port.id)},
                                              {QStringLiteral("name"), QString::fromStdString(port.name)}});
                 }
             }
             return ports;
         }()},
        {QStringLiteral("player"), [this] {
             const auto p = m_recordings->player();
             return QJsonObject{{QStringLiteral("path"), p.path},
                                {QStringLiteral("paused"), p.paused},
                                {QStringLiteral("duration"), p.duration},
                                {QStringLiteral("loop"), m_recordings->isLooping()}};
         }()},
        {QStringLiteral("playerTarget"), QString::fromStdString(m_recordings->playerTarget())},
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
        fail(QStringLiteral(
            "a virtual device has no device to bypass to; make another strip the default"));
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

void PatchPrimeService::SetDefaultInput(const QString &key, bool throughMixer)
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
    const auto kind = settings != nullptr ? settings->kind : pp::EndpointKind::Hardware;
    const bool isSource =
        (endpoint.direction == pp::Direction::Input && kind == pp::EndpointKind::Hardware) ||
        kind == pp::EndpointKind::VirtualSource;
    if (!isSource) {
        fail(QStringLiteral("not something apps record from: ") + key);
        return;
    }
    if (throughMixer && (settings == nullptr || !settings->enabled)) {
        fail(QStringLiteral("enable the device first: ") + key);
        return;
    }
    if (!throughMixer && kind != pp::EndpointKind::Hardware) {
        fail(QStringLiteral(
            "a virtual device has no device to bypass to; make another strip the default"));
        return;
    }
    const auto name = throughMixer ? pp::busNameFor(endpoint, *settings) : endpoint.nodeName;
    if (!m_graph->setConfiguredDefault(pp::Direction::Input, name)) {
        fail(QStringLiteral("PipeWire refused the default device change"));
    }
}

void PatchPrimeService::SetMono(const QString &key, bool mono)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setMono(row, mono);
    }
}

void PatchPrimeService::SetChannelPair(const QString &key, int pair)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setChannelPair(row, pair);
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

void PatchPrimeService::SetClock(const QString &mode)
{
    if (mode != QLatin1String("internal") && mode != QLatin1String("hardware")) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("clock must be \"internal\" or \"hardware\""));
        }
        return;
    }
    m_devices->setClock(pp::clockModeFromName(mode.toStdString()));
}

void PatchPrimeService::SetClockDevice(const QString &key)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setClockDevice(key.toStdString());
    }
}

QString PatchPrimeService::clockNow() const
{
    // The clock follows from the setting: Patch Prime's buses, or the chosen
    // device, outrank everything else as the graph driver. (PipeWire's own
    // node.driver-id isn't reliably updated on our side when it changes.)
    const auto &store = m_devices->store();
    if (store.clock() == pp::ClockMode::Internal) {
        return QStringLiteral("Patch Prime");
    }
    const int row = m_devices->rowForKey(store.clockDevice());
    if (row < 0) {
        return QStringLiteral("Patch Prime"); // no master chosen: the buses outrank the hardware
    }
    const auto index = m_devices->index(row);
    const auto label = m_devices->data(index, pp::DeviceModel::LabelRole).toString();
    // Unplugged: Patch Prime's buses outrank everything left, so they take
    // over until the master is back.
    return m_devices->data(index, pp::DeviceModel::OnlineRole).toBool()
               ? label
               : QStringLiteral("Patch Prime (%1 is unplugged)").arg(label);
}

void PatchPrimeService::SetSampleRate(int rate)
{
    static constexpr std::array rates{44100, 48000, 88200, 96000, 176400, 192000};
    if (std::ranges::find(rates, rate) == rates.end()) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("unsupported sample rate"));
        }
        return;
    }
    m_devices->setSampleRate(rate);
}

void PatchPrimeService::SetBlockSize(int frames)
{
    if (frames != 0 && (frames < 32 || frames > 8192 || (frames & (frames - 1)) != 0)) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::InvalidArgs,
                           QStringLiteral("block size must be 0 or a power of two, 32..8192"));
        }
        return;
    }
    m_devices->setBlockSize(frames);
}

void PatchPrimeService::SetDeviceSampleRate(const QString &key, int rate)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setDeviceSampleRate(row, std::max(rate, 0));
    }
}

void PatchPrimeService::SetDeviceScheduling(const QString &key, const QString &scheduling)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setDeviceScheduling(row, pp::schedulingFromName(scheduling.toStdString()));
    }
}

void PatchPrimeService::SetSendLevel(const QString &inputKey, const QString &destinationKey,
                                     double db)
{
    if (const int row = rowFor(inputKey); row >= 0) {
        m_devices->setSendLevel(row, destinationKey, db);
    }
}

void PatchPrimeService::AddEffect(const QString &key, const QString &type)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->addEffect(row, type);
    }
}

void PatchPrimeService::RemoveEffect(const QString &key, int index)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->removeEffect(row, index);
    }
}

void PatchPrimeService::MoveEffect(const QString &key, int from, int to)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->moveEffect(row, from, to);
    }
}

void PatchPrimeService::SetEffectEnabled(const QString &key, int index, bool enabled)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setEffectEnabled(row, index, enabled);
    }
}

void PatchPrimeService::SetEffectParam(const QString &key, int index, const QString &name,
                                       double value)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setEffectParam(row, index, name, value);
    }
}

QString PatchPrimeService::GetPlugins() const
{
    return QString::fromUtf8(pp::pluginsToJson(m_plugins->plugins()));
}

void PatchPrimeService::SetPluginWindow(const QString &key, int index, bool open)
{
    if (index >= 0) {
        m_buses->setPluginWindow(key.toStdString(), static_cast<std::size_t>(index), open);
    }
}

void PatchPrimeService::RescanPlugins()
{
    m_plugins->rescan();
    scheduleState();
}

QString PatchPrimeService::StartRecording(const QString &key)
{
    QString error;
    m_recordings->start(key.toStdString(), &error);
    return error;
}

void PatchPrimeService::StopRecording(const QString &key)
{
    m_recordings->stop(key.toStdString());
}

void PatchPrimeService::SetRecordingFormat(const QString &format)
{
    if (const auto parsed = pp::parseRecordingFormat(format.toStdString())) {
        m_recordings->setFormat(*parsed);
    }
}

QString PatchPrimeService::GetRecordings()
{
    QJsonArray files;
    for (const auto &f : m_recordings->files()) {
        files.append(QJsonObject{
            {QStringLiteral("path"), f.path},
            {QStringLiteral("name"), f.name},
            {QStringLiteral("seconds"), f.seconds},
            {QStringLiteral("bytes"), static_cast<double>(f.bytes)},
            {QStringLiteral("modified"), f.modified.toString(Qt::ISODate)},
        });
    }
    return QString::fromUtf8(QJsonDocument(files).toJson(QJsonDocument::Compact));
}

QString PatchPrimeService::PlayRecording(const QString &path)
{
    QString error;
    m_recordings->play(path, &error);
    return error;
}

void PatchPrimeService::PausePlayback(bool paused)
{
    m_recordings->setPaused(paused);
}

void PatchPrimeService::StopPlayback()
{
    m_recordings->stopPlayback();
}

void PatchPrimeService::SeekPlayback(double seconds)
{
    m_recordings->seek(seconds);
}

void PatchPrimeService::SetPlayerTarget(const QString &key)
{
    m_recordings->setPlayerTarget(key.toStdString());
}

void PatchPrimeService::SetPlaybackLoop(bool loop)
{
    m_recordings->setLoop(loop);
}

void PatchPrimeService::SetCueOutput(const QString &key)
{
    m_devices->setCueOutput(key.toStdString());
}

void PatchPrimeService::SetCue(const QString &key, bool cued)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setCue(row, cued);
    }
}

void PatchPrimeService::SetEffectKey(const QString &key, int index, const QString &sourceKey)
{
    if (const int row = rowFor(key); row >= 0) {
        m_devices->setEffectKey(row, index, sourceKey);
    }
}

QString PatchPrimeService::CreateVirtual(const QString &name, const QString &kind)
{
    const auto trimmed = name.trimmed();
    // "sink" / "source" / "input" here, "virtual-sink" etc. in the state.
    const auto type = pp::kindFromName("virtual-" + kind.toStdString());
    if (trimmed.isEmpty() || type == pp::EndpointKind::Hardware) {
        if (calledFromDBus()) {
            sendErrorReply(
                QDBusError::InvalidArgs,
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

void PatchPrimeService::SaveScene(const QString &name)
{
    const auto trimmed = name.trimmed();
    const auto scene = pp::captureScene(trimmed, m_devices->store(), m_apps->store(),
                                        m_graph->defaultSinkName(), m_graph->defaultSourceName());
    if (trimmed.isEmpty() || !m_scenes->save(scene)) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("could not save the scene ") + name);
        }
        return;
    }
    m_devices->setCurrentScene(trimmed.toStdString());
    scheduleState();
}

void PatchPrimeService::LoadScene(const QString &name)
{
    const auto scene = m_scenes->load(name);
    if (scene.isEmpty()) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no scene called ") + name);
        }
        return;
    }
    pp::applyScene(scene, *m_devices, m_apps, m_graph);
    m_devices->setCurrentScene(name.trimmed().toStdString());
    scheduleState();
}

void PatchPrimeService::DeleteScene(const QString &name)
{
    if (!m_scenes->remove(name)) {
        if (calledFromDBus()) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no scene called ") + name);
        }
        return;
    }
    if (m_devices->store().currentScene() == name.trimmed().toStdString()) {
        m_devices->setCurrentScene({});
    }
    scheduleState();
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
    if (!m_effectLevelWatchers.contains(caller)) {
        m_watcher.removeWatchedService(caller);
    }
    m_levelWatchers.remove(caller);
    updateMetering();
}

void PatchPrimeService::WatchEffectLevels(const QString &key)
{
    if (!calledFromDBus()) {
        return;
    }
    const auto caller = message().service();
    if (!m_levelWatchers.contains(caller) && !m_effectLevelWatchers.contains(caller)) {
        m_watcher.addWatchedService(caller);
    }
    m_effectLevelWatchers[caller].insert(key);
    updateEffectMetering();
}

void PatchPrimeService::UnwatchEffectLevels(const QString &key)
{
    if (!calledFromDBus()) {
        return;
    }
    const auto caller = message().service();
    if (auto it = m_effectLevelWatchers.find(caller); it != m_effectLevelWatchers.end()) {
        it->remove(key);
        if (it->isEmpty()) {
            m_effectLevelWatchers.erase(it);
            if (!m_levelWatchers.contains(caller)) {
                m_watcher.removeWatchedService(caller);
            }
        }
    }
    updateEffectMetering();
}

void PatchPrimeService::updateEffectMetering()
{
    std::set<std::string> keys;
    for (const auto &watched : std::as_const(m_effectLevelWatchers)) {
        for (const auto &key : watched) {
            keys.insert(key.toStdString());
        }
    }
    m_buses->setEffectMetering(std::move(keys));
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
    m_buses->setMetering(watched || (m_mackie != nullptr && m_mackie->wantsMeters()));
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
    for (const auto &[key, values] : m_buses->loudness()) {
        const auto name = QString::fromStdString(key);
        levels.insert(QStringLiteral("rms|") + name,
                      QVariant::fromValue(QList<double>(values.rms.begin(), values.rms.end())));
        levels.insert(QStringLiteral("lufs|") + name,
                      QVariant::fromValue(QList<double>{static_cast<double>(values.lufs)}));
    }
    for (const auto &[at, meters] : m_buses->takeEffectMeters()) {
        QList<double> values(meters.begin(), meters.end());
        levels.insert(QStringLiteral("fx|%1|%2").arg(QString::fromStdString(at.first)).arg(at.second),
                      QVariant::fromValue(values));
    }
    for (const auto &[at, io] : m_buses->takeEffectLevels()) {
        const auto groups = static_cast<qsizetype>(io.groups);
        QList<double> values;
        values.reserve(groups * 4);
        for (const auto *part : {&io.inPeak, &io.inRms, &io.outPeak, &io.outRms}) {
            for (qsizetype g = 0; g < groups; ++g) {
                values.append(static_cast<double>(part->at(static_cast<std::size_t>(g))));
            }
        }
        if (io.hasKey) {
            values.append(static_cast<double>(io.keyPeak));
            values.append(static_cast<double>(io.keyRms));
        }
        levels.insert(
            QStringLiteral("fxio|%1|%2").arg(QString::fromStdString(at.first)).arg(at.second),
            QVariant::fromValue(values));
    }
    for (const auto &[key, seconds] : m_recordings->recordedSeconds()) {
        levels.insert(QStringLiteral("rec|") + QString::fromStdString(key),
                      QVariant::fromValue(QList<double>{seconds}));
    }
    if (const auto player = m_recordings->player(); !player.path.isEmpty()) {
        levels.insert(QStringLiteral("player"), QVariant::fromValue(QList<double>{player.position}));
    }
    if (!levels.isEmpty()) {
        Q_EMIT Levels(levels);
    }
}

void PatchPrimeService::SetControlSurface(const QString &id)
{
    m_devices->setControlSurface(id.trimmed().toStdString());
}

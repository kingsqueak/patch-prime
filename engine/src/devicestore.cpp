// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicestore.h"

#include "effects.h"
#include "plugins.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

constexpr int formatVersion = 1;

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString().toStdString();
}

QJsonObject toJson(const DeviceSettings &d)
{
    QJsonArray positions;
    for (const auto &p : d.positions) {
        positions.append(qs(p));
    }
    QJsonArray sends;
    for (const auto &s : d.sends) {
        sends.append(qs(s));
    }
    QJsonObject sendLevels;
    for (const auto &[key, db] : d.sendLevels) {
        sendLevels.insert(qs(key), db);
    }
    return QJsonObject{
        {QStringLiteral("kind"), qs(kindName(d.kind))},
        {QStringLiteral("sends"), sends},
        {QStringLiteral("sendLevels"), sendLevels},
        {QStringLiteral("effects"), effectsToJson(d.effects)},
        {QStringLiteral("mono"), d.mono},
        {QStringLiteral("channelPair"), d.channelPair},
        {QStringLiteral("sampleRate"), d.sampleRate},
        {QStringLiteral("scheduling"), qs(schedulingName(d.scheduling))},
        {QStringLiteral("busId"), qs(d.busId)},
        {QStringLiteral("busName"), qs(d.busName)},
        {QStringLiteral("enabled"), d.enabled},
        {QStringLiteral("label"), qs(d.label)},
        {QStringLiteral("volumeDb"), d.volumeDb},
        {QStringLiteral("muted"), d.muted},
        {QStringLiteral("positions"), positions},
        {QStringLiteral("nodeName"), qs(d.nodeName)},
        {QStringLiteral("mediaClass"), qs(d.mediaClass)},
        {QStringLiteral("direction"),
         d.direction == Direction::Input ? QStringLiteral("input") : QStringLiteral("output")},
        {QStringLiteral("virtual"), d.isVirtual},
        {QStringLiteral("description"), qs(d.description)},
        {QStringLiteral("channels"), d.channels},
        {QStringLiteral("lastSeen"), d.lastSeen.toString(Qt::ISODate)},
        {QStringLiteral("signature"),
         QJsonObject{
             {QStringLiteral("serial"), qs(d.signature.deviceSerial)},
             {QStringLiteral("vendorId"), qs(d.signature.vendorId)},
             {QStringLiteral("productId"), qs(d.signature.productId)},
             {QStringLiteral("busPath"), qs(d.signature.busPath)},
             {QStringLiteral("profile"), qs(d.signature.profile)},
         }},
    };
}

DeviceSettings fromJson(const QJsonObject &o)
{
    DeviceSettings d;
    d.kind = kindFromName(str(o, "kind"));
    d.mono = o.value(QLatin1String("mono")).toBool();
    d.channelPair = std::max(0, o.value(QLatin1String("channelPair")).toInt());
    d.sampleRate = std::max(0, o.value(QLatin1String("sampleRate")).toInt());
    d.scheduling = schedulingFromName(str(o, "scheduling"));
    for (const auto &s : o.value(QLatin1String("sends")).toArray()) {
        d.sends.push_back(s.toString().toStdString());
    }
    const auto sendLevels = o.value(QLatin1String("sendLevels")).toObject();
    for (auto it = sendLevels.begin(); it != sendLevels.end(); ++it) {
        d.sendLevels[it.key().toStdString()] = it.value().toDouble();
    }
    d.effects = effectsFromJson(o.value(QLatin1String("effects")).toArray());
    d.busId = str(o, "busId");
    d.busName = str(o, "busName");
    d.enabled = o.value(QLatin1String("enabled")).toBool();
    d.label = str(o, "label");
    d.volumeDb = o.value(QLatin1String("volumeDb")).toDouble(0.0);
    d.muted = o.value(QLatin1String("muted")).toBool();
    for (const auto &p : o.value(QLatin1String("positions")).toArray()) {
        d.positions.push_back(p.toString().toStdString());
    }
    d.nodeName = str(o, "nodeName");
    d.mediaClass = str(o, "mediaClass");
    d.direction = str(o, "direction") == "input" ? Direction::Input : Direction::Output;
    d.isVirtual = o.value(QLatin1String("virtual")).toBool();
    d.description = str(o, "description");
    d.channels = o.value(QLatin1String("channels")).toInt();
    d.lastSeen = QDateTime::fromString(o.value(QLatin1String("lastSeen")).toString(), Qt::ISODate);
    const auto sig = o.value(QLatin1String("signature")).toObject();
    d.signature.deviceSerial = str(sig, "serial");
    d.signature.vendorId = str(sig, "vendorId");
    d.signature.productId = str(sig, "productId");
    d.signature.busPath = str(sig, "busPath");
    d.signature.profile = str(sig, "profile");
    return d;
}

} // namespace

std::string kindName(EndpointKind kind)
{
    switch (kind) {
    case EndpointKind::VirtualSink:
        return "virtual-sink";
    case EndpointKind::VirtualSource:
        return "virtual-source";
    case EndpointKind::VirtualInput:
        return "virtual-input";
    case EndpointKind::Hardware:
        break;
    }
    return "hardware";
}

EndpointKind kindFromName(std::string_view name)
{
    if (name == "virtual-sink") {
        return EndpointKind::VirtualSink;
    }
    if (name == "virtual-source") {
        return EndpointKind::VirtualSource;
    }
    if (name == "virtual-input") {
        return EndpointKind::VirtualInput;
    }
    return EndpointKind::Hardware;
}

Stage stageOf(Direction direction, EndpointKind kind)
{
    if (direction == Direction::Input) {
        return Stage::Input;
    }
    return kind == EndpointKind::VirtualSink ? Stage::Bus : Stage::Output;
}

std::string stageName(Stage stage)
{
    switch (stage) {
    case Stage::Input:
        return "input";
    case Stage::Bus:
        return "bus";
    case Stage::Output:
        break;
    }
    return "output";
}

std::string schedulingName(Scheduling scheduling)
{
    switch (scheduling) {
    case Scheduling::Timer:
        return "timer";
    case Scheduling::Interrupt:
        return "interrupt";
    case Scheduling::Auto:
        break;
    }
    return "auto";
}

Scheduling schedulingFromName(std::string_view name)
{
    if (name == "timer") {
        return Scheduling::Timer;
    }
    if (name == "interrupt") {
        return Scheduling::Interrupt;
    }
    return Scheduling::Auto;
}

std::string alsaCardOf(std::string_view nodeName)
{
    for (const std::string_view prefix : {"alsa_input.", "alsa_output."}) {
        if (nodeName.starts_with(prefix)) {
            nodeName.remove_prefix(prefix.size());
            const auto dot = nodeName.rfind('.');
            return std::string(dot == std::string_view::npos ? nodeName : nodeName.substr(0, dot));
        }
    }
    return {};
}

std::string clockModeName(ClockMode mode)
{
    return mode == ClockMode::Hardware ? "hardware" : "internal";
}

ClockMode clockModeFromName(std::string_view name)
{
    return name == "hardware" ? ClockMode::Hardware : ClockMode::Internal;
}

DeviceStore::DeviceStore(QString path) : m_path(path.isEmpty() ? defaultPath() : std::move(path)) {}

QString DeviceStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/patchprime/devices.json");
}

bool DeviceStore::load()
{
    m_entries.clear();
    m_clock = ClockMode::Internal;
    m_clockDevice.clear();
    m_cueOutput.clear();
    m_sampleRate = defaultSampleRate;
    m_blockSize = 0;
    m_recordingFormat = "flac";
    m_playerTarget.clear();
    m_currentScene.clear();
    m_controlSurface.clear();
    m_midiController.clear();
    m_midiMappings.clear();
    m_lastError.clear();
    QFile file(m_path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        m_lastError = parseError.errorString();
        return false;
    }
    const auto root = document.object();
    m_clock = clockModeFromName(root.value(QLatin1String("clock")).toString().toStdString());
    m_clockDevice = root.value(QLatin1String("clockDevice")).toString().toStdString();
    m_cueOutput = root.value(QLatin1String("cueOutput")).toString().toStdString();
    m_sampleRate = root.value(QLatin1String("sampleRate")).toInt(defaultSampleRate);
    m_blockSize = std::max(0, root.value(QLatin1String("blockSize")).toInt());
    m_recordingFormat =
        root.value(QLatin1String("recordingFormat")).toString(QStringLiteral("flac")).toStdString();
    m_playerTarget = root.value(QLatin1String("playerTarget")).toString().toStdString();
    m_currentScene = root.value(QLatin1String("currentScene")).toString().toStdString();
    m_controlSurface = root.value(QLatin1String("controlSurface")).toString().toStdString();
    m_midiController = root.value(QLatin1String("midiController")).toString().toStdString();
    for (const auto &m : root.value(QLatin1String("midiMappings")).toArray()) {
        if (auto mapping = midiMappingFromJson(m.toObject())) {
            m_midiMappings.push_back(std::move(*mapping));
        }
    }
    const auto devices = document.object().value(QLatin1String("devices")).toObject();
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        auto settings = fromJson(it.value().toObject());
        if (settings.busId.empty()) {
            settings.busId = newBusId(); // files written before Phase 2
        }
        m_entries[it.key().toStdString()] = std::move(settings);
    }
    return true;
}

bool DeviceStore::save()
{
    m_lastError.clear();
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_lastError = QStringLiteral("could not create ") + QFileInfo(m_path).absolutePath();
        return false;
    }
    QJsonObject devices;
    for (const auto &[key, settings] : m_entries) {
        devices.insert(qs(key), toJson(settings));
    }
    const QJsonObject root{
        {QStringLiteral("version"), formatVersion},
        {QStringLiteral("clock"), qs(clockModeName(m_clock))},
        {QStringLiteral("clockDevice"), qs(m_clockDevice)},
        {QStringLiteral("cueOutput"), qs(m_cueOutput)},
        {QStringLiteral("sampleRate"), m_sampleRate},
        {QStringLiteral("blockSize"), m_blockSize},
        {QStringLiteral("recordingFormat"), qs(m_recordingFormat)},
        {QStringLiteral("playerTarget"), qs(m_playerTarget)},
        {QStringLiteral("currentScene"), qs(m_currentScene)},
        {QStringLiteral("controlSurface"), qs(m_controlSurface)},
        {QStringLiteral("midiController"), qs(m_midiController)},
        {QStringLiteral("midiMappings"), [this] {
             QJsonArray mappings;
             for (const auto &m : m_midiMappings) {
                 mappings.append(midiMappingToJson(m));
             }
             return mappings;
         }()},
        {QStringLiteral("devices"), devices},
    };

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
}

const DeviceSettings *DeviceStore::find(const std::string &key) const
{
    const auto it = m_entries.find(key);
    return it != m_entries.end() ? &it->second : nullptr;
}

DeviceSettings &DeviceStore::upsert(const std::string &key)
{
    auto &entry = m_entries[key];
    if (entry.busId.empty()) {
        entry.busId = newBusId();
    }
    return entry;
}

std::string DeviceStore::newBusId()
{
    // 8 hex digits: short enough for node names, and collisions among a few
    // dozen devices are not a concern.
    return QUuid::createUuid().toString(QUuid::Id128).left(8).toStdString();
}

bool DeviceStore::remove(const std::string &key)
{
    if (m_entries.erase(key) == 0) {
        return false;
    }
    for (auto &[other, settings] : m_entries) {
        std::erase(settings.sends, key);
        settings.sendLevels.erase(key);
    }
    std::erase_if(m_midiMappings, [&key](const MidiMapping &m) {
        return m.target.key == key || m.target.destination == key;
    });
    return true;
}

bool DeviceStore::rekey(const std::string &from, const std::string &to)
{
    auto node = m_entries.extract(from);
    if (node.empty() || m_entries.contains(to)) {
        if (!node.empty()) {
            m_entries.insert(std::move(node));
        }
        return false;
    }
    node.key() = to;
    m_entries.insert(std::move(node));
    if (m_clockDevice == from) {
        m_clockDevice = to;
    }
    if (m_playerTarget == from) {
        m_playerTarget = to;
    }
    if (m_cueOutput == from) {
        m_cueOutput = to;
    }
    for (auto &m : m_midiMappings) {
        if (m.target.key == from) {
            m.target.key = to;
        }
        if (m.target.destination == from) {
            m.target.destination = to;
        }
    }
    for (auto &[key, settings] : m_entries) {
        std::ranges::replace(settings.sends, from, to);
        if (auto level = settings.sendLevels.extract(from)) {
            level.key() = to;
            settings.sendLevels.insert(std::move(level));
        }
    }
    return true;
}

QJsonObject mixToJson(const DeviceSettings &settings)
{
    QJsonArray sends;
    for (const auto &s : settings.sends) {
        sends.append(qs(s));
    }
    QJsonObject sendLevels;
    for (const auto &[key, db] : settings.sendLevels) {
        sendLevels.insert(qs(key), db);
    }
    return {
        {QStringLiteral("volumeDb"), settings.volumeDb},
        {QStringLiteral("muted"), settings.muted},
        {QStringLiteral("mono"), settings.mono},
        {QStringLiteral("sends"), sends},
        {QStringLiteral("sendLevels"), sendLevels},
        {QStringLiteral("effects"), effectsToJson(settings.effects)},
    };
}

void mixFromJson(DeviceSettings &settings, const QJsonObject &json)
{
    if (const auto v = json.value(QLatin1String("volumeDb")); v.isDouble()) {
        settings.volumeDb = v.toDouble();
    }
    if (const auto v = json.value(QLatin1String("muted")); v.isBool()) {
        settings.muted = v.toBool();
    }
    if (const auto v = json.value(QLatin1String("mono")); v.isBool()) {
        settings.mono = v.toBool();
    }
    if (const auto v = json.value(QLatin1String("sends")); v.isArray()) {
        settings.sends.clear();
        for (const auto &s : v.toArray()) {
            settings.sends.push_back(s.toString().toStdString());
        }
    }
    if (const auto v = json.value(QLatin1String("sendLevels")); v.isObject()) {
        settings.sendLevels.clear();
        const auto levels = v.toObject();
        for (auto it = levels.begin(); it != levels.end(); ++it) {
            settings.sendLevels[it.key().toStdString()] = it.value().toDouble();
        }
    }
    if (const auto v = json.value(QLatin1String("effects")); v.isArray()) {
        settings.effects = effectsFromJson(v.toArray());
    }
}

QJsonArray effectsToJson(const std::vector<EffectSettings> &effects)
{
    QJsonArray out;
    for (const auto &e : effects) {
        QJsonObject params;
        for (const auto &[name, value] : e.params) {
            params.insert(qs(name), value);
        }
        QJsonObject effect{
            {QStringLiteral("type"), qs(e.type)},
            {QStringLiteral("enabled"), e.enabled},
            {QStringLiteral("params"), params},
        };
        if (effectTakesKey(e.type)) {
            effect.insert(QStringLiteral("key"), qs(e.key));
        }
        if (!e.state.empty()) {
            effect.insert(QStringLiteral("state"), qs(e.state));
        }
        out.append(effect);
    }
    return out;
}

std::vector<EffectSettings> effectsFromJson(const QJsonArray &json)
{
    std::vector<EffectSettings> effects;
    for (const auto &v : json) {
        const auto o = v.toObject();
        EffectSettings e;
        e.type = str(o, "type");
        e.enabled = o.value(QLatin1String("enabled")).toBool(true);
        const auto params = o.value(QLatin1String("params")).toObject();
        if (isPluginType(e.type)) {
            // A plugin's parameters are its own (checked against the plugin
            // when they're changed): kept as they are, if they're numbers.
            for (auto it = params.begin(); it != params.end(); ++it) {
                if (const double number = it.value().toDouble(qQNaN()); std::isfinite(number)) {
                    e.params[it.key().toStdString()] = number;
                }
            }
            e.state = str(o, "state");
            effects.push_back(std::move(e));
            continue;
        }
        const auto info = effectParams(e.type);
        if (info.empty()) {
            continue; // unknown (or from a newer version)
        }
        for (const auto &p : info) {
            const double value = params.value(QString::fromStdString(p.name)).toDouble(p.initial);
            e.params[p.name] = validParam(p, value);
        }
        if (effectTakesKey(e.type)) {
            e.key = str(o, "key");
        }
        effects.push_back(std::move(e));
    }
    return effects;
}

bool isPatchPrimeVirtual(std::string_view key)
{
    return key.starts_with("patchprime-virtual|");
}

std::string DeviceStore::virtualKey(const std::string &busId)
{
    return "patchprime-virtual|" + busId;
}

} // namespace pp

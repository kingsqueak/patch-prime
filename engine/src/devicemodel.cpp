// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"

#include "busmanager.h"
#include "effects.h"
#include "pipewiregraph.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <tuple>
#include <utility>

Q_LOGGING_CATEGORY(lcDevices, "patchprime.devices")

namespace pp {

namespace {

const std::string nodeType = "PipeWire:Interface:Node";
const std::string deviceType = "PipeWire:Interface:Device";

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::optional<std::uint32_t> parseId(const std::string &value)
{
    std::uint32_t id = 0;
    if (value.empty() ||
        std::from_chars(value.data(), value.data() + value.size(), id).ec != std::errc{}) {
        return std::nullopt;
    }
    return id;
}

Endpoint endpointFromSettings(const std::string &key, const DeviceSettings &s)
{
    Endpoint e;
    e.key = key;
    e.nodeName = s.nodeName;
    e.mediaClass = s.mediaClass;
    e.direction = s.direction;
    e.isVirtual = s.isVirtual;
    e.description = s.description.empty() ? s.nodeName : s.description;
    e.channels = s.channels;
    e.positions = s.positions;
    e.signature = s.signature;
    return e;
}

} // namespace

DeviceModel::DeviceModel(DeviceStore *store, QObject *parent)
    : QAbstractListModel(parent), m_store(store)
{
    for (const auto &[key, settings] : m_store->entries()) {
        m_rows.push_back({.endpoint = endpointFromSettings(key, settings), .nodeId = {}});
    }
    std::ranges::sort(m_rows, &DeviceModel::lessThan);

    // Fader moves arrive many times a second; write the file once they settle.
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &DeviceModel::saveStore);
}

DeviceModel::~DeviceModel()
{
    if (m_saveTimer.isActive()) {
        saveStore(); // a fader move still waiting to be written
    }
}

void DeviceModel::attach(PipeWireGraph *graph)
{
    m_graph = graph;

    auto deviceFor = [graph](const GraphObject &node) -> const GraphObject * {
        const auto id = parseId(prop(node, "device.id"));
        if (!id) {
            return nullptr;
        }
        const auto it = graph->devices().find(*id);
        return it != graph->devices().end() ? &it->second : nullptr;
    };
    auto onAddedOrChanged = [this, graph, deviceFor](const GraphObject &object) {
        if (object.type == nodeType) {
            upsertNode(object, deviceFor(object));
        } else if (object.type == deviceType) {
            // The signature comes from the card, so refresh its nodes.
            const auto id = std::to_string(object.id);
            for (const auto &[nodeId, node] : graph->nodes()) {
                if (prop(node, "device.id") == id) {
                    upsertNode(node, &object);
                }
            }
        }
    };

    connect(graph, &PipeWireGraph::objectAdded, this, onAddedOrChanged);
    connect(graph, &PipeWireGraph::objectChanged, this, onAddedOrChanged);
    connect(graph, &PipeWireGraph::objectRemoved, this, [this](const GraphObject &object) {
        if (object.type == nodeType) {
            removeNode(object.id);
        }
    });
    connect(graph, &PipeWireGraph::defaultsChanged, this,
            [this, graph] { setDefaults(graph->defaultSinkName(), graph->defaultSourceName()); });
    connect(graph, &PipeWireGraph::connectedChanged, this, &DeviceModel::setConnected);

    setConnected(graph->isConnected());
    for (const auto &[id, node] : graph->nodes()) {
        upsertNode(node, deviceFor(node));
    }
    setDefaults(graph->defaultSinkName(), graph->defaultSourceName());
}

int DeviceModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant DeviceModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const auto &row = m_rows[static_cast<std::size_t>(index.row())];
    const auto &e = row.endpoint;
    const auto *saved = m_store->find(e.key);

    switch (role) {
    case Qt::DisplayRole:
    case LabelRole:
        return qs(saved != nullptr && !saved->label.empty() ? saved->label : e.description);
    case KeyRole:
        return qs(e.key);
    case CustomLabelRole:
        return qs(saved != nullptr ? saved->label : std::string{});
    case DescriptionRole:
        return qs(e.description);
    case NodeNameRole:
        return qs(e.nodeName);
    case DirectionRole:
        return e.direction == Direction::Input ? QStringLiteral("input") : QStringLiteral("output");
    case VirtualRole:
        return e.isVirtual;
    case OnlineRole:
        return row.nodeId.has_value();
    case EnabledRole:
        return saved != nullptr && saved->enabled;
    case ChannelsRole:
        return e.channels;
    case PositionsRole: {
        QStringList positions;
        for (const auto &p : e.positions) {
            positions.append(qs(p));
        }
        return positions.join(QLatin1Char(' '));
    }
    case DefaultRole:
        return isDefault(row);
    case SavedRole:
        return saved != nullptr;
    case VolumeRole:
        return saved != nullptr ? saved->volumeDb : 0.0;
    case MutedRole:
        return saved != nullptr && saved->muted;
    case BusIdRole:
        return qs(saved != nullptr ? saved->busId : std::string{});
    case KindRole:
        return qs(kindName(saved != nullptr ? saved->kind : EndpointKind::Hardware));
    case SendsRole: {
        QStringList sends;
        if (saved != nullptr) {
            for (const auto &s : saved->sends) {
                sends.append(qs(s));
            }
        }
        return sends;
    }
    case MonoRole:
        return saved != nullptr && saved->mono;
    case ChannelPairRole:
        return saved != nullptr ? saved->channelPair : 0;
    case SendLevelsRole: {
        QVariantMap levels;
        if (saved != nullptr) {
            for (const auto &[key, db] : saved->sendLevels) {
                levels.insert(qs(key), db);
            }
        }
        return levels;
    }
    case DeviceRateRole:
        return saved != nullptr ? saved->sampleRate : 0;
    case CuedRole:
        return m_cued.contains(e.key);
    case EffectsRole:
        return saved != nullptr ? effectsToJson(saved->effects).toVariantList() : QVariantList{};
    case SchedulingRole:
        return qs(schedulingName(saved != nullptr ? saved->scheduling : Scheduling::Auto));
    case StageRole:
        return qs(stageName(stageOfRow(row)));
    case BusNameRole:
        return qs(saved != nullptr && saved->enabled ? busNameFor(e, *saved) : std::string{});
    case BusDefaultRole: {
        // The strip's bus is the system default output (a sink apps play
        // into) or the system default input (a source apps record from).
        if (saved == nullptr || !saved->enabled) {
            return false;
        }
        const auto busClass = busMediaClass(e, *saved);
        const std::string *name = nullptr;
        if (busClass == "Audio/Sink") {
            name = &m_defaultSink;
        } else if (busClass.starts_with("Audio/Source")) {
            name = &m_defaultSource;
        }
        return name != nullptr && !name->empty() && *name == busNameFor(e, *saved);
    }
    case SectionStartRole:
        return index.row() == 0 ||
               m_rows[static_cast<std::size_t>(index.row()) - 1].endpoint.direction != e.direction;
    default:
        return {};
    }
}

QHash<int, QByteArray> DeviceModel::roleNames() const
{
    return {
        {KeyRole, "key"},
        {LabelRole, "label"},
        {CustomLabelRole, "customLabel"},
        {DescriptionRole, "description"},
        {NodeNameRole, "nodeName"},
        {DirectionRole, "direction"},
        {VirtualRole, "isVirtual"},
        {OnlineRole, "online"},
        {EnabledRole, "isEnabled"},
        {ChannelsRole, "channels"},
        {PositionsRole, "positions"},
        {DefaultRole, "isDefault"},
        {SavedRole, "saved"},
        {SectionStartRole, "sectionStart"},
        {VolumeRole, "volumeDb"},
        {MutedRole, "muted"},
        {BusIdRole, "busId"},
        {BusDefaultRole, "busIsDefault"},
        {KindRole, "kind"},
        {BusNameRole, "busName"},
        {StageRole, "stage"},
        {SendLevelsRole, "sendLevels"},
        {DeviceRateRole, "deviceRate"},
        {SchedulingRole, "scheduling"},
        {EffectsRole, "effects"},
        {CuedRole, "cued"},
        {ChannelPairRole, "channelPair"},
        {SendsRole, "sends"},
        {MonoRole, "mono"},
    };
}

void DeviceModel::setEnabled(int row, bool enabled)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).enabled = enabled;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {EnabledRole, SavedRole, BusIdRole, BusNameRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

void DeviceModel::setVolume(int row, double volumeDb)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    volumeDb = std::clamp(volumeDb, minimumVolumeDb(), maximumVolumeDb());
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    if (const auto *saved = m_store->find(r.endpoint.key);
        saved != nullptr && saved->volumeDb == volumeDb) {
        return;
    }
    m_store->upsert(r.endpoint.key).volumeDb = volumeDb;
    saveLater();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {VolumeRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

void DeviceModel::setMuted(int row, bool muted)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).muted = muted;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {MutedRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

void DeviceModel::setLabel(int row, const QString &label)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    const auto trimmed = label.trimmed().toStdString();
    const auto *saved = m_store->find(r.endpoint.key);
    if ((saved == nullptr && trimmed.empty()) || (saved != nullptr && saved->label == trimmed)) {
        return;
    }
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).label = trimmed;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {LabelRole, CustomLabelRole, Qt::DisplayRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

void DeviceModel::forget(int row)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    const auto key = qs(r.endpoint.key);
    if (!m_store->remove(r.endpoint.key)) {
        return;
    }
    m_cued.erase(r.endpoint.key);
    if (m_store->cueOutput() == r.endpoint.key) {
        m_store->setCueOutput({});
        m_cued.clear();
        Q_EMIT cueChanged();
    }
    saveStore();
    Q_EMIT settingsChanged(key);
    // A virtual device exists only through its entry, so it goes entirely.
    const bool isOwnVirtual = isPatchPrimeVirtual(r.endpoint.key);
    if (r.nodeId && !isOwnVirtual) {
        const auto i = index(row);
        Q_EMIT dataChanged(i, i);
    } else {
        removeRowAt(row);
    }
    // Inputs that were sent here lost that send.
    if (!m_rows.empty()) {
        Q_EMIT dataChanged(index(0), index(rowCount() - 1), {SendsRole});
    }
}

void DeviceModel::setMono(int row, bool mono)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    if (r.endpoint.direction != Direction::Input) {
        return;
    }
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).mono = mono;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {MonoRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

void DeviceModel::setClock(ClockMode clock)
{
    if (clock == m_store->clock()) {
        return;
    }
    m_store->setClock(clock);
    saveStore();
    Q_EMIT clockChanged();
    Q_EMIT audioSettingsChanged(); // Auto scheduling depends on the clock
}

void DeviceModel::setClockDevice(const std::string &key)
{
    const auto *saved = m_store->find(key);
    // Only an ALSA device can be made the clock (through WirePlumber's ALSA rules).
    if (saved == nullptr || saved->kind != EndpointKind::Hardware ||
        !saved->nodeName.starts_with("alsa_") ||
        (m_store->clock() == ClockMode::Hardware && m_store->clockDevice() == key)) {
        return;
    }
    m_store->setClock(ClockMode::Hardware);
    m_store->setClockDevice(key);
    saveStore();
    Q_EMIT clockChanged();
    Q_EMIT audioSettingsChanged();
}

void DeviceModel::setRecordingFormat(const std::string &format)
{
    if (format == m_store->recordingFormat()) {
        return;
    }
    m_store->setRecordingFormat(format);
    saveStore();
    Q_EMIT recordingSettingsChanged();
}

void DeviceModel::setPlayerTarget(const std::string &key)
{
    if (key == m_store->playerTarget()) {
        return;
    }
    m_store->setPlayerTarget(key);
    saveStore();
    Q_EMIT recordingSettingsChanged();
}

void DeviceModel::setCueOutput(const std::string &key)
{
    if (!key.empty()) {
        const auto *saved = m_store->find(key);
        if (saved == nullptr || saved->kind != EndpointKind::Hardware ||
            saved->direction != Direction::Output) {
            return; // cue plays to a hardware output's device directly
        }
    }
    if (key == m_store->cueOutput()) {
        return;
    }
    m_store->setCueOutput(key);
    saveStore();
    if (key.empty() && !m_cued.empty()) {
        m_cued.clear();
        if (!m_rows.empty()) {
            Q_EMIT dataChanged(index(0), index(rowCount() - 1), {CuedRole});
        }
    }
    Q_EMIT cueChanged();
}

void DeviceModel::setCue(int row, bool cued)
{
    if (row < 0 || row >= rowCount() || m_store->cueOutput().empty()) {
        return;
    }
    const auto &key = m_rows[static_cast<std::size_t>(row)].endpoint.key;
    if (key == m_store->cueOutput() || m_cued.contains(key) == cued) {
        return;
    }
    if (cued) {
        m_cued.insert(key);
    } else {
        m_cued.erase(key);
    }
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {CuedRole});
    Q_EMIT cueChanged();
}

void DeviceModel::setSampleRate(int rate)
{
    if (rate <= 0 || rate == m_store->sampleRate()) {
        return;
    }
    m_store->setSampleRate(rate);
    saveStore();
    Q_EMIT audioSettingsChanged();
}

void DeviceModel::setBlockSize(int frames)
{
    if (frames < 0 || frames == m_store->blockSize()) {
        return;
    }
    m_store->setBlockSize(frames);
    saveStore();
    Q_EMIT audioSettingsChanged();
}

void DeviceModel::setDeviceSampleRate(int row, int rate)
{
    if (row < 0 || row >= rowCount() || rate < 0) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    if (isPatchPrimeVirtual(r.endpoint.key)) {
        return; // hardware only
    }
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).sampleRate = rate;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {DeviceRateRole, SavedRole});
    Q_EMIT audioSettingsChanged();
}

void DeviceModel::setDeviceScheduling(int row, Scheduling scheduling)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    if (isPatchPrimeVirtual(r.endpoint.key)) {
        return; // hardware only
    }
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).scheduling = scheduling;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {SchedulingRole, SavedRole});
    Q_EMIT audioSettingsChanged();
}

void DeviceModel::setSendLevel(int row, const QString &destinationKey, double db)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    const auto *saved = m_store->find(r.endpoint.key);
    const auto destination = destinationKey.toStdString();
    if (saved == nullptr || std::ranges::find(saved->sends, destination) == saved->sends.end()) {
        return; // only for a send that exists
    }
    db = std::clamp(db, minimumVolumeDb(), maximumVolumeDb());
    auto &levels = m_store->upsert(r.endpoint.key).sendLevels;
    if (const auto it = levels.find(destination);
        it != levels.end() ? it->second == db : db == 0.0) {
        return;
    }
    levels[destination] = db;
    saveLater();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {SendLevelsRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

bool DeviceModel::editEffects(int row, const std::function<bool(std::vector<EffectSettings> &)> &edit)
{
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    auto &effects = m_store->upsert(r.endpoint.key).effects;
    if (!edit(effects)) {
        return false;
    }
    saveLater();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {EffectsRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
    return true;
}

void DeviceModel::addEffect(int row, const QString &type)
{
    const auto params = effectParams(type.toStdString());
    if (params.empty()) {
        return;
    }
    editEffects(row, [&](std::vector<EffectSettings> &effects) {
        if (effects.size() >= maxEffectsPerStrip) {
            return false;
        }
        EffectSettings e{.type = type.toStdString(), .enabled = true, .params = {}, .key = {}};
        for (const auto &p : params) {
            e.params[p.name] = p.initial;
        }
        effects.push_back(std::move(e));
        return true;
    });
}

void DeviceModel::removeEffect(int row, int index)
{
    editEffects(row, [index](std::vector<EffectSettings> &effects) {
        if (index < 0 || std::cmp_greater_equal(index, effects.size())) {
            return false;
        }
        effects.erase(effects.begin() + index);
        return true;
    });
}

void DeviceModel::moveEffect(int row, int from, int to)
{
    editEffects(row, [from, to](std::vector<EffectSettings> &effects) {
        const auto size = static_cast<int>(effects.size());
        if (from < 0 || from >= size || to < 0 || to >= size || from == to) {
            return false;
        }
        auto moved = std::move(effects[static_cast<std::size_t>(from)]);
        effects.erase(effects.begin() + from);
        effects.insert(effects.begin() + to, std::move(moved));
        return true;
    });
}

void DeviceModel::setEffectEnabled(int row, int index, bool enabled)
{
    editEffects(row, [index, enabled](std::vector<EffectSettings> &effects) {
        if (index < 0 || std::cmp_greater_equal(index, effects.size())) {
            return false;
        }
        auto &e = effects[static_cast<std::size_t>(index)];
        if (e.enabled == enabled) {
            return false;
        }
        e.enabled = enabled;
        return true;
    });
}

void DeviceModel::setEffectParam(int row, int index, const QString &name, double value)
{
    editEffects(row, [index, &name, value](std::vector<EffectSettings> &effects) {
        if (index < 0 || std::cmp_greater_equal(index, effects.size())) {
            return false;
        }
        auto &e = effects[static_cast<std::size_t>(index)];
        const auto key = name.toStdString();
        for (const auto &p : effectParams(e.type)) {
            if (p.name == key) {
                const double clamped = validParam(p, value);
                if (e.params[key] == clamped) {
                    return false;
                }
                e.params[key] = clamped;
                return true;
            }
        }
        return false;
    });
}

void DeviceModel::setEffectKey(int row, int index, const QString &key)
{
    const auto source = key.toStdString();
    if (row < 0 || row >= rowCount() ||
        (!source.empty() && (m_store->find(source) == nullptr ||
                             source == m_rows[static_cast<std::size_t>(row)].endpoint.key))) {
        return; // unknown, or the strip itself
    }
    editEffects(row, [index, &source](std::vector<EffectSettings> &effects) {
        if (index < 0 || std::cmp_greater_equal(index, effects.size())) {
            return false;
        }
        auto &e = effects[static_cast<std::size_t>(index)];
        if (!effectTakesKey(e.type) || e.key == source) {
            return false;
        }
        e.key = source;
        return true;
    });
}

bool DeviceModel::applyMix(const std::string &key, const QJsonObject &mix)
{
    const int row = rowForKey(key);
    if (row < 0) {
        return false;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    m_store->upsert(key);
    remember(r, r.nodeId.has_value());
    auto &s = m_store->upsert(key);
    DeviceSettings next = s;
    mixFromJson(next, mix);

    s.volumeDb = std::clamp(next.volumeDb, minimumVolumeDb(), maximumVolumeDb());
    s.muted = next.muted;
    s.mono = r.endpoint.direction == Direction::Input && next.mono;
    const auto stage = stageOfRow(r);
    s.sends.clear();
    for (const auto &destination : next.sends) {
        const int to = rowForKey(destination);
        if (to >= 0 && stageOfRow(m_rows[static_cast<std::size_t>(to)]) > stage &&
            std::ranges::find(s.sends, destination) == s.sends.end()) {
            s.sends.push_back(destination);
        }
    }
    s.sendLevels.clear();
    for (const auto &[destination, db] : next.sendLevels) {
        s.sendLevels[destination] = std::clamp(db, minimumVolumeDb(), maximumVolumeDb());
    }
    s.effects = std::move(next.effects);
    if (s.effects.size() > maxEffectsPerStrip) {
        s.effects.resize(maxEffectsPerStrip);
    }
    saveLater();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i,
                       {VolumeRole, MutedRole, MonoRole, SendsRole, SendLevelsRole, EffectsRole,
                        SavedRole});
    Q_EMIT settingsChanged(qs(key));
    return true;
}

void DeviceModel::setChannelPair(int row, int pair)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    if (isPatchPrimeVirtual(r.endpoint.key) || pair < 0 ||
        pair * 2 > r.endpoint.channels || (pair > 0 && r.endpoint.channels <= 2)) {
        return;
    }
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    m_store->upsert(r.endpoint.key).channelPair = pair;
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {ChannelPairRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

void DeviceModel::setSend(int row, const QString &destinationKey, bool enabled)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    const auto destination = destinationKey.toStdString();
    const int destinationRow = rowForKey(destination);
    if (destinationRow < 0 ||
        stageOfRow(r) >= stageOfRow(m_rows[static_cast<std::size_t>(destinationRow)])) {
        return; // only to a later stage: that's also what keeps loops impossible
    }
    m_store->upsert(r.endpoint.key);
    remember(r, r.nodeId.has_value());
    auto &sends = m_store->upsert(r.endpoint.key).sends;
    const bool has = std::ranges::find(sends, destination) != sends.end();
    if (has == enabled) {
        return;
    }
    if (enabled) {
        sends.push_back(destination);
    } else {
        std::erase(sends, destination);
    }
    saveStore();
    const auto i = index(row);
    Q_EMIT dataChanged(i, i, {SendsRole, SavedRole});
    Q_EMIT settingsChanged(qs(r.endpoint.key));
}

QString DeviceModel::createVirtual(const QString &name, EndpointKind kind)
{
    if (kind == EndpointKind::Hardware) {
        return {};
    }
    const auto busId = DeviceStore::newBusId();
    const auto key = DeviceStore::virtualKey(busId);
    auto &s = m_store->upsert(key);
    s.busId = busId;
    s.kind = kind;
    s.enabled = true;
    s.label = name.trimmed().toStdString();
    s.busName = readableBusName(*m_store, key);
    s.nodeName = s.busName; // a virtual device is its own bus
    s.mediaClass = kind == EndpointKind::VirtualSource ? "Audio/Source/Virtual" : "Audio/Sink";
    // A virtual input feeds sends like a hardware input; the others are destinations.
    s.direction = kind == EndpointKind::VirtualInput ? Direction::Input : Direction::Output;
    s.isVirtual = true;
    s.description = s.label;
    s.channels = 2;
    s.positions = {"FL", "FR"};
    s.lastSeen = QDateTime::currentDateTimeUtc();
    saveStore();
    insertSorted({.endpoint = endpointFromSettings(key, s), .nodeId = {}});
    Q_EMIT settingsChanged(qs(key));
    return qs(key);
}

void DeviceModel::upsertNode(const GraphObject &node, const GraphObject *device)
{
    // Our own nodes (buses, virtual devices: tagged with patchprime.key): a
    // virtual device is online once its node exists; buses are no rows.
    if (!prop(node, "patchprime.key").empty()) {
        const auto &name = prop(node, "node.name");
        const auto it = std::ranges::find_if(
            m_rows, [&name](const Row &r) { return r.endpoint.nodeName == name; });
        if (it != m_rows.end() && it->nodeId != node.id) {
            it->nodeId = node.id;
            const auto i = index(static_cast<int>(it - m_rows.begin()));
            Q_EMIT dataChanged(i, i, {OnlineRole, DefaultRole, BusDefaultRole});
        }
        return;
    }
    const auto endpoint = classifyEndpoint(node, device);
    if (const int current = rowForNode(node.id);
        current >= 0 &&
        (!endpoint || m_rows[static_cast<std::size_t>(current)].endpoint.key != endpoint->key)) {
        // The node stopped being an endpoint, or was renamed in place.
        removeNode(node.id);
    }
    if (!endpoint) {
        return;
    }

    Row row{.endpoint = *endpoint, .nodeId = node.id};

    if (const int existing = rowForKey(endpoint->key); existing >= 0) {
        const bool cameOnline = !m_rows[static_cast<std::size_t>(existing)].nodeId.has_value();
        replaceRow(existing, row);
        remember(row, cameOnline);
        return;
    }

    if (const int candidate = findReattachCandidate(*endpoint); candidate >= 0) {
        const auto old = m_rows[static_cast<std::size_t>(candidate)].endpoint;
        if (m_store->rekey(old.key, endpoint->key)) {
            const auto *saved = m_store->find(endpoint->key);
            const auto label = qs(saved->label.empty() ? old.description : saved->label);
            qCInfo(lcDevices, "recognised %s as %s", old.nodeName.c_str(),
                   endpoint->nodeName.c_str());
            replaceRow(candidate, row);
            remember(row, true);
            Q_EMIT settingsChanged(qs(old.key));
            Q_EMIT settingsChanged(qs(endpoint->key));
            Q_EMIT reattached(label, qs(old.nodeName), qs(endpoint->nodeName));
            return;
        }
    }

    insertSorted(std::move(row));
}

void DeviceModel::removeNode(std::uint32_t nodeId)
{
    const int index = rowForNode(nodeId);
    if (index < 0) {
        return;
    }
    auto &row = m_rows[static_cast<std::size_t>(index)];
    if (m_store->find(row.endpoint.key) != nullptr) {
        row.nodeId.reset();
        const auto i = this->index(index);
        Q_EMIT dataChanged(i, i, {OnlineRole, DefaultRole});
    } else {
        removeRowAt(index);
    }
}

void DeviceModel::setDefaults(const std::string &sinkName, const std::string &sourceName)
{
    if (sinkName == m_defaultSink && sourceName == m_defaultSource) {
        return;
    }
    m_defaultSink = sinkName;
    m_defaultSource = sourceName;
    if (!m_rows.empty()) {
        Q_EMIT dataChanged(index(0), index(rowCount() - 1), {DefaultRole, BusDefaultRole});
    }
}

void DeviceModel::setConnected(bool connected)
{
    if (connected != m_connected) {
        m_connected = connected;
        Q_EMIT connectedChanged();
    }
}

int DeviceModel::rowForKey(const std::string &key) const
{
    const auto it =
        std::ranges::find_if(m_rows, [&key](const Row &r) { return r.endpoint.key == key; });
    return it != m_rows.end() ? static_cast<int>(it - m_rows.begin()) : -1;
}

int DeviceModel::rowForNode(std::uint32_t nodeId) const
{
    const auto it =
        std::ranges::find_if(m_rows, [nodeId](const Row &r) { return r.nodeId == nodeId; });
    return it != m_rows.end() ? static_cast<int>(it - m_rows.begin()) : -1;
}

int DeviceModel::findReattachCandidate(const Endpoint &live) const
{
    if (live.isVirtual) {
        return -1; // virtual endpoints have no hardware signature; only exact names match
    }
    int best = -1;
    bool tie = false;
    std::tuple<int, bool, bool> bestRank{0, false, false};

    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const auto &row = m_rows[i];
        const auto &saved = row.endpoint;
        if (row.nodeId || saved.isVirtual || saved.mediaClass != live.mediaClass ||
            m_store->find(saved.key) == nullptr) {
            continue;
        }
        const auto match = matchSignature(saved.signature, live.signature);
        if (match == Match::None) {
            continue;
        }
        const bool sameProfile = saved.signature.profile == live.signature.profile;
        // A bare model match is a device moved to another port, which keeps its
        // profile. Without that, it's too weak to act on.
        if (match == Match::SameModel && !sameProfile) {
            continue;
        }
        const std::tuple rank{static_cast<int>(match),
                              saved.signature.busPath == live.signature.busPath, sameProfile};
        if (rank > bestRank) {
            bestRank = rank;
            best = static_cast<int>(i);
            tie = false;
        } else if (rank == bestRank) {
            tie = true;
        }
    }
    return tie ? -1 : best;
}

Stage DeviceModel::stageOfRow(const Row &row) const
{
    const auto *saved = m_store->find(row.endpoint.key);
    return stageOf(row.endpoint.direction, saved != nullptr ? saved->kind : EndpointKind::Hardware);
}

bool DeviceModel::isDefault(const Row &row) const
{
    if (!row.nodeId) {
        return false;
    }
    // A virtual input is a sink, so it's the default output, not input.
    const auto &name = playsInto(row.endpoint) ? m_defaultSink : m_defaultSource;
    return !name.empty() && name == row.endpoint.nodeName;
}

bool DeviceModel::lessThan(const Row &a, const Row &b)
{
    const auto rank = [](const Row &r) {
        return std::tuple{r.endpoint.direction != Direction::Input, r.endpoint.isVirtual,
                          lower(r.endpoint.description), r.endpoint.key};
    };
    return rank(a) < rank(b);
}

void DeviceModel::insertSorted(Row row)
{
    const auto it = std::ranges::upper_bound(m_rows, row, &DeviceModel::lessThan);
    const int position = static_cast<int>(it - m_rows.begin());
    beginInsertRows({}, position, position);
    m_rows.insert(it, std::move(row));
    endInsertRows();
    refreshSectionStarts();
}

void DeviceModel::replaceRow(int index, Row row)
{
    const auto i = static_cast<std::size_t>(index);
    const bool staysSorted = (i == 0 || !lessThan(row, m_rows[i - 1])) &&
                             (i + 1 == m_rows.size() || !lessThan(m_rows[i + 1], row));
    if (staysSorted) {
        m_rows[i] = std::move(row);
        const auto modelIndex = this->index(index);
        Q_EMIT dataChanged(modelIndex, modelIndex);
        return;
    }
    removeRowAt(index);
    insertSorted(std::move(row));
}

void DeviceModel::removeRowAt(int index)
{
    beginRemoveRows({}, index, index);
    m_rows.erase(m_rows.begin() + index);
    endRemoveRows();
    refreshSectionStarts();
}

// The list is short, so every row is refreshed rather than working out which
// neighbours changed.
void DeviceModel::refreshSectionStarts()
{
    if (!m_rows.empty()) {
        Q_EMIT dataChanged(index(0), index(rowCount() - 1), {SectionStartRole});
    }
}

void DeviceModel::remember(const Row &row, bool seenNow)
{
    if (m_store->find(row.endpoint.key) == nullptr) {
        return;
    }
    auto &s = m_store->upsert(row.endpoint.key);
    const auto &e = row.endpoint;
    const DeviceSettings before = s;
    s.nodeName = e.nodeName;
    s.mediaClass = e.mediaClass;
    s.direction = e.direction;
    s.isVirtual = e.isVirtual;
    s.description = e.description;
    s.channels = e.channels;
    s.positions = e.positions;
    // An offline row carries the saved signature; only a live one refreshes it.
    if (row.nodeId) {
        s.signature = e.signature;
    }
    const bool detailsChanged =
        s.nodeName != before.nodeName || s.mediaClass != before.mediaClass ||
        s.description != before.description || s.channels != before.channels ||
        s.positions != before.positions || !(s.signature == before.signature);
    if (seenNow) {
        s.lastSeen = QDateTime::currentDateTimeUtc();
    }
    const bool named = s.busName.empty();
    if (named) {
        s.busName = readableBusName(*m_store, row.endpoint.key); // a new strip's bus name
    }
    if (detailsChanged || seenNow || named) {
        saveStore();
    }
}

void DeviceModel::saveLater()
{
    m_saveTimer.start();
}

void DeviceModel::saveStore()
{
    m_saveTimer.stop();
    if (!m_store->save()) {
        qCWarning(lcDevices, "could not save %s: %s", qPrintable(m_store->path()),
                  qPrintable(m_store->lastError()));
    }
}

} // namespace pp

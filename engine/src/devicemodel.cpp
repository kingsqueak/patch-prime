// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"

#include "busmanager.h"
#include "pipewiregraph.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <tuple>

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
    case BusNameRole:
        return qs(saved != nullptr && saved->enabled ? busNameFor(e, *saved) : std::string{});
    case BusDefaultRole:
        return saved != nullptr && saved->enabled && playsInto(e) && !m_defaultSink.empty() &&
               m_defaultSink == busNameFor(e, *saved);
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
    saveStore();
    Q_EMIT settingsChanged(key);
    // A virtual device exists only through its entry, so it goes entirely.
    const bool isOwnVirtual = r.endpoint.nodeName.starts_with(virtualNamePrefix);
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

void DeviceModel::setChannelPair(int row, int pair)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto &r = m_rows[static_cast<std::size_t>(row)];
    if (r.endpoint.nodeName.starts_with(virtualNamePrefix) || pair < 0 ||
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
    if (r.endpoint.direction != Direction::Input || destinationRow < 0 ||
        m_rows[static_cast<std::size_t>(destinationRow)].endpoint.direction != Direction::Output) {
        return; // only input -> output: that's also what keeps loops impossible
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
    s.nodeName = virtualNodeName(busId);
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
    // Our own virtual devices: online once their node exists.
    if (const auto &name = prop(node, "node.name"); name.starts_with(virtualNamePrefix)) {
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
    if (detailsChanged || seenNow) {
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

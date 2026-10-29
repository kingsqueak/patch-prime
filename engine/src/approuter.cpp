// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "approuter.h"

#include "pipewiregraph.h"

#include <QLoggingCategory>

#include <algorithm>
#include <set>

Q_LOGGING_CATEGORY(lcApps, "patchprime.apps")

namespace pp {

namespace {

constexpr std::string_view nodeType = "PipeWire:Interface:Node";
constexpr std::string_view systemDefault = "-1";

bool isNumber(const std::string &value)
{
    return !value.empty() && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

std::optional<AppStream> classifyAppStream(const GraphObject &node)
{
    if (node.type != nodeType) {
        return std::nullopt;
    }
    const auto &mediaClass = prop(node, "media.class");
    StreamDirection direction{};
    if (mediaClass == "Stream/Output/Audio") {
        direction = StreamDirection::Playback;
    } else if (mediaClass == "Stream/Input/Audio") {
        direction = StreamDirection::Record;
    } else {
        return std::nullopt;
    }
    const auto &nodeName = prop(node, "node.name");
    if (nodeName.starts_with("patchprime.") || prop(node, "node.passive") == "true" ||
        prop(node, "stream.monitor") == "true" || !prop(node, "node.link-group").empty()) {
        return std::nullopt;
    }
    const auto &binary = prop(node, "application.process.binary");
    const auto &appName = prop(node, "application.name");
    std::string id = binary;
    for (const auto *fallback : {&appName, &nodeName}) {
        if (id.empty()) {
            id = *fallback;
        }
    }
    if (id.empty()) {
        return std::nullopt;
    }
    const auto &icon = prop(node, "application.icon-name");
    return AppStream{
        .key = streamDirectionName(direction) + "|" + id,
        .name = appName.empty() ? id : appName,
        .icon = icon.empty() ? binary : icon,
        .direction = direction,
    };
}

AppRouter::AppRouter(PipeWireGraph *graph, AppStore *store, QObject *parent)
    : QObject(parent), m_graph(graph), m_store(store)
{
    m_reconcileTimer.setSingleShot(true);
    m_reconcileTimer.setInterval(20);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &AppRouter::reconcile);

    auto onNode = [this](const GraphObject &object) {
        if (object.type == nodeType) {
            scheduleReconcile();
        }
    };
    connect(graph, &PipeWireGraph::objectAdded, this, onNode);
    connect(graph, &PipeWireGraph::objectChanged, this, onNode);
    connect(graph, &PipeWireGraph::objectRemoved, this, onNode);
    connect(graph, &PipeWireGraph::connectedChanged, this, &AppRouter::scheduleReconcile);
    connect(graph, &PipeWireGraph::streamTargetChanged, this, &AppRouter::onStreamTargetChanged);
    scheduleReconcile();
}

std::vector<AppRouter::App> AppRouter::apps() const
{
    std::map<std::string, App> out;
    for (const auto &[key, settings] : m_store->entries()) {
        out[key] = App{.key = key, .settings = settings, .streams = 0};
    }
    for (const auto &[id, live] : m_streams) {
        auto &app = out[live.app.key];
        if (app.key.empty()) { // running, but still settling: not remembered yet
            app.key = live.app.key;
            app.settings.direction = live.app.direction;
            app.settings.name = live.app.name;
            app.settings.icon = live.app.icon;
            app.settings.target = nodeNameFor(m_graph->streamTarget(id)).value_or(std::string{});
        }
        ++app.streams;
    }
    std::vector<App> list;
    list.reserve(out.size());
    for (auto &[key, app] : out) {
        list.push_back(std::move(app));
    }
    return list;
}

bool AppRouter::setTarget(const std::string &key, const std::string &nodeName)
{
    const auto live = std::ranges::find_if(
        m_streams, [&key](const auto &entry) { return entry.second.app.key == key; });
    if (m_store->find(key) == nullptr && live == m_streams.end()) {
        return false;
    }
    if (m_store->find(key) == nullptr) {
        remember(live->second.app, nodeName);
    } else {
        m_store->upsert(key).target = nodeName;
    }
    qCInfo(lcApps, "routing %s to %s", key.c_str(),
           nodeName.empty() ? "the system default" : nodeName.c_str());
    save();
    reconcile();
    Q_EMIT changed();
    return true;
}

bool AppRouter::forget(const std::string &key)
{
    const bool running = std::ranges::any_of(
        m_streams, [&key](const auto &entry) { return entry.second.app.key == key; });
    if (running || !m_store->remove(key)) {
        return false;
    }
    save();
    Q_EMIT changed();
    return true;
}

void AppRouter::scheduleReconcile()
{
    if (!m_reconcileTimer.isActive()) {
        m_reconcileTimer.start();
    }
}

void AppRouter::reconcile()
{
    const auto now = Clock::now();
    bool listChanged = false;

    // Follow the streams that come and go.
    std::set<std::uint32_t> present;
    if (m_graph->isConnected()) {
        for (const auto &[id, node] : m_graph->nodes()) {
            auto app = classifyAppStream(node);
            if (!app) {
                continue;
            }
            present.insert(id);
            if (const auto it = m_streams.find(id);
                it == m_streams.end() || it->second.app.key != app->key) {
                m_streams[id] = Live{.app = std::move(*app), .seen = now};
                m_written.erase(id);
                listChanged = true;
            }
        }
    }
    listChanged |= std::erase_if(m_streams, [&present](const auto &entry) {
                       return !present.contains(entry.first);
                   }) > 0;
    std::erase_if(m_written, [&present](const auto &entry) { return !present.contains(entry.first); });

    bool settling = false;
    for (const auto &[id, live] : m_streams) {
        const auto *settings = m_store->find(live.app.key);
        if (settings == nullptr) {
            if (now - live.seen < settleTime) {
                settling = true; // let WirePlumber restore its own target first
                continue;
            }
            // A new app: remember wherever WirePlumber put it.
            remember(live.app, nodeNameFor(m_graph->streamTarget(id)).value_or(std::string{}));
            save();
            listChanged = true;
            continue;
        }
        const auto wanted = targetValueFor(settings->target);
        if (!wanted) {
            continue; // the target isn't there (a disabled strip, an unplugged card)
        }
        if (m_graph->streamTarget(id) != *wanted && m_written[id] != *wanted) {
            qCInfo(lcApps, "moving %s (stream %u) to %s", live.app.key.c_str(), id,
                   settings->target.empty() ? "the system default" : settings->target.c_str());
            if (m_graph->setStreamTarget(id, *wanted)) {
                m_written[id] = *wanted;
            }
        }
    }
    if (settling) {
        QTimer::singleShot(settleTime, this, &AppRouter::scheduleReconcile);
    }
    if (listChanged) {
        Q_EMIT changed();
    }
}

void AppRouter::onStreamTargetChanged(std::uint32_t nodeId)
{
    const auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) {
        scheduleReconcile(); // a stream we haven't seen yet
        return;
    }
    const auto value = m_graph->streamTarget(nodeId);
    const auto written = m_written.find(nodeId);
    if (written != m_written.end() && written->second == value) {
        return; // our own write coming back
    }
    // A target cleared (not a move), or WirePlumber restoring a new stream's
    // old target: not the user's choice, so the route is written again.
    if (value.empty() || Clock::now() - it->second.seen < settleTime) {
        m_written.erase(nodeId);
        scheduleReconcile();
        return;
    }
    // Moved elsewhere (Plasma's applet, pavucontrol): that's the route now.
    const auto *settings = m_store->find(it->second.app.key);
    const auto target = nodeNameFor(value);
    m_written[nodeId] = value;
    if (target && (settings == nullptr || settings->target != *target)) {
        qCInfo(lcApps, "%s was moved to %s elsewhere; keeping that", it->second.app.key.c_str(),
               target->empty() ? "the system default" : target->c_str());
        remember(it->second.app, *target);
        save();
        Q_EMIT changed();
    }
    scheduleReconcile(); // the app's other streams follow
}

std::optional<std::string> AppRouter::nodeNameFor(const std::string &targetValue) const
{
    if (targetValue.empty() || targetValue == systemDefault) {
        return std::string{};
    }
    if (!isNumber(targetValue)) {
        return targetValue; // a node.name
    }
    for (const auto &[id, node] : m_graph->nodes()) {
        if (prop(node, "object.serial") == targetValue) {
            return prop(node, "node.name");
        }
    }
    return std::nullopt;
}

std::optional<std::string> AppRouter::targetValueFor(const std::string &nodeName) const
{
    if (nodeName.empty()) {
        return std::string(systemDefault);
    }
    for (const auto &[id, node] : m_graph->nodes()) {
        if (prop(node, "node.name") == nodeName) {
            const auto &serial = prop(node, "object.serial");
            return serial.empty() ? std::nullopt : std::optional{serial};
        }
    }
    return std::nullopt;
}

void AppRouter::remember(const AppStream &app, const std::string &target)
{
    auto &settings = m_store->upsert(app.key);
    settings.direction = app.direction;
    settings.name = app.name;
    settings.icon = app.icon;
    settings.target = target;
    settings.lastSeen = QDateTime::currentDateTimeUtc();
}

void AppRouter::save()
{
    if (!m_store->save()) {
        qCWarning(lcApps, "could not save %s: %s", qPrintable(m_store->path()),
                  qPrintable(m_store->lastError()));
    }
}

} // namespace pp

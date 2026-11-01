// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "appmixer.h"

#include "approuter.h"
#include "pipewiregraph.h"

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

double toDb(float linear)
{
    return linear <= 0.0F ? AppMixer::minimumDb
                          : std::max(AppMixer::minimumDb, 20.0 * std::log10(static_cast<double>(linear)));
}

float toLinear(double db)
{
    return db <= AppMixer::minimumDb ? 0.0F : static_cast<float>(std::pow(10.0, std::min(db, AppMixer::maximumDb) / 20.0));
}

} // namespace

AppMixer::AppMixer(PipeWireGraph *graph, AppRouter *router, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_router(router)
{
    connect(m_graph, &PipeWireGraph::nodeVolumeChanged, this, [this](std::uint32_t id) {
        if (!appOf(id).empty()) {
            Q_EMIT changed();
        }
    });
    connect(m_router, &AppRouter::changed, this, [this] {
        reconcileMeters();
        Q_EMIT changed();
    });
    connect(m_graph, &PipeWireGraph::connectedChanged, this, [this] {
        m_meters.clear(); // meters belong to the old connection
        reconcileMeters();
    });
}

AppMixer::~AppMixer()
{
    setMetering(false);
}

std::string AppMixer::appOf(std::uint32_t nodeId) const
{
    for (const auto &app : m_router->apps()) {
        if (std::ranges::contains(m_router->streamNodes(app.key), nodeId)) {
            return app.key;
        }
    }
    return {};
}

std::optional<double> AppMixer::volumeDb(const std::string &key) const
{
    for (const auto id : m_router->streamNodes(key)) {
        if (const auto v = m_graph->nodeVolume(id); v && !v->channels.empty()) {
            return toDb(std::ranges::max(v->channels));
        }
    }
    return std::nullopt;
}

std::optional<bool> AppMixer::isMuted(const std::string &key) const
{
    for (const auto id : m_router->streamNodes(key)) {
        if (const auto v = m_graph->nodeVolume(id)) {
            return v->mute;
        }
    }
    return std::nullopt;
}

bool AppMixer::setVolume(const std::string &key, double db)
{
    return apply(key, db, std::nullopt);
}

bool AppMixer::setMuted(const std::string &key, bool muted)
{
    return apply(key, std::nullopt, muted);
}

bool AppMixer::apply(const std::string &key, const std::optional<double> &db, const std::optional<bool> &muted)
{
    bool any = false;
    for (const auto id : m_router->streamNodes(key)) {
        const auto now = m_graph->nodeVolume(id);
        std::size_t channels = now ? now->channels.size() : 0;
        if (channels == 0) {
            const auto node = m_graph->nodes().find(id);
            channels = node == m_graph->nodes().end()
                           ? 2
                           : static_cast<std::size_t>(std::max(1, std::atoi(prop(node->second, "audio.channels").c_str())));
        }
        // Keep the other half: a new level keeps the mute, and the other way round.
        std::vector<float> volumes(channels, db ? toLinear(*db) : 1.0F);
        if (!db && now) {
            volumes = now->channels;
        }
        any = m_graph->setNodeVolume(id, volumes, muted.value_or(now && now->mute)) || any;
    }
    return any;
}

void AppMixer::setMetering(bool enabled)
{
    if (enabled == m_metering) {
        return;
    }
    m_metering = enabled;
    reconcileMeters();
}

void AppMixer::reconcileMeters()
{
    std::map<std::uint32_t, std::string> wanted;
    if (m_metering && m_graph->isConnected()) {
        for (const auto &app : m_router->apps()) {
            if (app.settings.direction != StreamDirection::Playback) {
                continue;
            }
            for (const auto id : m_router->streamNodes(app.key)) {
                wanted[id] = app.key;
            }
        }
    }
    for (auto it = m_meters.begin(); it != m_meters.end();) {
        if (!wanted.contains(it->first)) {
            m_graph->removeMeter(it->second.levels);
            it = m_meters.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &[id, app] : wanted) {
        if (m_meters.contains(id)) {
            continue;
        }
        const auto node = m_graph->nodes().find(id);
        if (node == m_graph->nodes().end()) {
            continue;
        }
        // Captured straight from the stream (its serial: app streams often
        // share a name), after its volume.
        const auto serial = prop(node->second, "object.serial");
        const auto &declared = prop(node->second, "audio.channels"); // not every stream says
        const int channels = declared.empty() ? 2 : std::clamp(std::atoi(declared.c_str()), 1, 8);
        if (auto levels = m_graph->addMeter(serial.empty() ? std::to_string(id) : serial, channels, false)) {
            m_meters[id] = Meter{.app = app, .levels = std::move(levels)};
        }
    }
}

std::map<std::string, std::vector<float>> AppMixer::takeLevels()
{
    std::map<std::string, std::vector<float>> out;
    for (const auto &[id, meter] : m_meters) {
        const auto peaks = meter.levels->take();
        auto &app = out[meter.app];
        if (app.size() < peaks.size()) {
            app.resize(peaks.size(), 0.0F);
        }
        for (std::size_t i = 0; i < peaks.size(); ++i) {
            app[i] = std::max(app[i], peaks[i]);
        }
    }
    return out;
}

} // namespace pp

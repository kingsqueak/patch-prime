// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QObject>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pp {

class AppRouter;
class PipeWireGraph;
struct MeterLevels;

// The mixer's app strips: an app's level and mute are its streams' own
// volume (what Plasma's volume applet shows and WirePlumber remembers per
// app), set on every stream the app has; its meter is the loudest of them,
// after that volume.
class AppMixer : public QObject
{
    Q_OBJECT

public:
    AppMixer(PipeWireGraph *graph, AppRouter *router, QObject *parent = nullptr);
    ~AppMixer() override;
    AppMixer(const AppMixer &) = delete;
    AppMixer &operator=(const AppMixer &) = delete;
    AppMixer(AppMixer &&) = delete;
    AppMixer &operator=(AppMixer &&) = delete;

    // The app's level in dB (minimumDb for silent) and mute; nullopt while
    // it has no stream whose volume is known.
    [[nodiscard]] std::optional<double> volumeDb(const std::string &key) const;
    [[nodiscard]] std::optional<bool> isMuted(const std::string &key) const;
    // On every stream of the app; false when it has none.
    bool setVolume(const std::string &key, double db);
    bool setMuted(const std::string &key, bool muted);

    // Meters cost a little CPU: only while someone watches.
    void setMetering(bool enabled);
    // Peaks per app key since the last call (linear, per channel).
    std::map<std::string, std::vector<float>> takeLevels();

    static constexpr double minimumDb = -60.0;
    static constexpr double maximumDb = 12.0; // as the mixer's faders

Q_SIGNALS:
    // An app's level or mute changed (from here, an app, or Plasma).
    void changed();

private:
    struct Meter
    {
        std::string app;
        std::shared_ptr<MeterLevels> levels;
    };

    void reconcileMeters();
    // The app a stream node belongs to, or empty.
    [[nodiscard]] std::string appOf(std::uint32_t nodeId) const;
    bool apply(const std::string &key, const std::optional<double> &db, const std::optional<bool> &muted);

    PipeWireGraph *m_graph;
    AppRouter *m_router;
    bool m_metering = false;
    std::map<std::uint32_t, Meter> m_meters; // by stream node
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "appstore.h"
#include "graphtypes.h"

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pp {

class PipeWireGraph;

// --- Pure helpers (unit tested) ---------------------------------------------

// An application's audio stream, as the app matrix sees it.
struct AppStream
{
    std::string key;  // "playback|<binary>" or "record|<binary>"
    std::string name; // application.name, else the binary
    std::string icon; // application.icon-name, else the binary
    StreamDirection direction = StreamDirection::Playback;
};

// Turns a node into an AppStream when it's an application's playback or
// recording stream. Patch Prime's own streams (meters), monitors and peak
// detectors (passive), and the inner streams of loopbacks and filters
// (node.link-group) return nullopt. Apps are identified by
// application.process.binary, else application.name, else node.name, so every
// stream of one program shares a route.
std::optional<AppStream> classifyAppStream(const GraphObject &node);

// --- AppRouter ---------------------------------------------------------------

// Sends each application's streams where the user chose, and remembers it.
//
// Routing is WirePlumber's own per-stream "target.object" metadata, set to the
// target node's object.serial ("-1" for the system default). It's what Plasma
// and pavucontrol write when an app is moved, and WirePlumber saves it and
// restores it when the app comes back, so the two agree. A move made elsewhere
// (Plasma's applet, say) is adopted as the app's new route rather than undone.
//
// Every app seen is remembered with where it went. A new stream gets a moment
// (settleTime) for WirePlumber to restore its own saved target before the
// app's route is applied, so the route wins over an older saved target.
class AppRouter : public QObject
{
    Q_OBJECT

public:
    // scope: only streams whose "patchprime.scope" property equals it are
    // handled; the daemon's is empty (streams without one). A test tags its
    // own streams so a running daemon leaves them alone, and vice versa.
    AppRouter(PipeWireGraph *graph, AppStore *store, std::string scope = {},
              QObject *parent = nullptr);

    struct App
    {
        std::string key;
        AppSettings settings;
        int streams = 0; // live streams; 0 when the app isn't running
    };
    // Every remembered app and every running one, by key.
    [[nodiscard]] std::vector<App> apps() const;

    // nodeName: the node.name the app's streams should go to; empty for the
    // system default. Returns false for an unknown app.
    bool setTarget(const std::string &key, const std::string &nodeName);
    // Forgets a remembered app that isn't running.
    bool forget(const std::string &key);

    static constexpr std::chrono::milliseconds settleTime{1500};

Q_SIGNALS:
    void changed();

private:
    using Clock = std::chrono::steady_clock;

    struct Live
    {
        AppStream app;
        Clock::time_point seen;
    };

    void scheduleReconcile();
    void reconcile();
    void onStreamTargetChanged(std::uint32_t nodeId);
    // "-1" and nothing mean the system default (""); a serial or a name means
    // that node; nullopt when the serial names no node.
    [[nodiscard]] std::optional<std::string> nodeNameFor(const std::string &targetValue) const;
    // The target the stream itself asks for (target.object, else node.target
    // in its props), or empty.
    [[nodiscard]] std::string ownTarget(std::uint32_t nodeId) const;
    [[nodiscard]] std::optional<std::string> targetValueFor(const std::string &nodeName) const;
    void remember(const AppStream &app, const std::string &target);
    void save();

    PipeWireGraph *m_graph;
    AppStore *m_store;
    std::string m_scope;
    QTimer m_reconcileTimer;
    std::map<std::uint32_t, Live> m_streams;
    // The target.object value last written to each stream, so our own
    // writes coming back aren't taken for a move made elsewhere.
    std::map<std::uint32_t, std::string> m_written;
};

} // namespace pp

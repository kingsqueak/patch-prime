// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "effects.h"

#include <QObject>
#include <QProcess>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class QTimer;

namespace pp {

// What one plugin node should run.
struct PluginSpec
{
    std::string type;                   // "lv2:<uri>" or "clap:<id>"
    std::string binary;                 // CLAP: the .clap file
    std::vector<std::string> positions; // the strip's channels
    int sampleRate = 48000;
    bool enabled = true; // false: bypassed
    std::map<std::string, double> params;
    std::string description; // the node's
    std::string owner;       // patchprime.fx.owner, as on the engine's effects nodes
    std::string title;       // its window's
    std::string state;       // the plugin's own state, restored when it starts

    bool operator==(const PluginSpec &) const = default;
};

// Runs each plugin node in a patchprime-pluginhost process of its own:
//
//   patchprime-pluginhost --run --format F --uri U [--binary B] --name N
//                         --positions FL,FR --rate R --owner O
//                         --description D --title T [--bypassed]
//
// makes a filter node N (like the engine's effects nodes) running plugin U.
// The runner talks to it in JSON lines. To it: {"params", "state"} first
// (what the plugin starts with), then {"set": {"enabled", "params"}},
// {"metering": bool}, {"window": bool} (its own window) and
// {"saveState": true}. From it: {"ready"} (its node is up), {"levels": ...},
// {"error": message}, {"window": bool}, {"windowError": message},
// {"param": {"name", "value"}} (changed in its window) and {"state": ...}
// (its own state changed). The host exits when its stdin closes (sending
// its state first), so it never outlives the engine.
//
// A host that crashes, or doesn't bring its node up within readyTimeout, is
// restarted after a second; after crashLimit of those within crashWindow its
// plugin stays off, with an error, until its spec changes. One that can't
// load its plugin (exit code 2) stays off too.
class PluginRunner : public QObject
{
    Q_OBJECT

public:
    static constexpr int crashLimit = 3;
    static constexpr auto crashWindow = std::chrono::seconds(60);
    static constexpr auto restartDelay = std::chrono::milliseconds(1000);
    static constexpr auto readyTimeout = std::chrono::seconds(20);
    // How long a host may take to exit when told to (then it's killed).
    static constexpr auto stopTimeout = std::chrono::milliseconds(800);
    // More unparsed output than this from a host means it's broken.
    static constexpr qsizetype maxPending = qsizetype{32} << 20;

    explicit PluginRunner(QObject *parent = nullptr);
    ~PluginRunner() override;
    PluginRunner(const PluginRunner &) = delete;
    PluginRunner &operator=(const PluginRunner &) = delete;
    PluginRunner(PluginRunner &&) = delete;
    PluginRunner &operator=(PluginRunner &&) = delete;

    // Runs (or updates) the plugin node named node. True when its host was
    // (re)started just now: a node of that name in the graph is the old one,
    // on its way out.
    bool want(const std::string &node, const PluginSpec &spec);
    // Stops every host whose node isn't in keep.
    void prune(const std::set<std::string> &keep);

    // Why the node's plugin is off ("" while it runs or starts).
    [[nodiscard]] std::string error(const std::string &node) const;
    // The plugin's input and output levels, measured while metering is on.
    void setMetering(const std::string &node, bool on);
    std::optional<EffectLevels> takeLevels(const std::string &node);
    // The plugin's own window: open (or bring forward) or close; whether
    // it's open, and why it couldn't be opened the last time. Asked while
    // the host is (re)starting, it's done once its node is up.
    void setWindow(const std::string &node, bool open);
    [[nodiscard]] bool isWindowOpen(const std::string &node) const;
    [[nodiscard]] std::string windowError(const std::string &node) const;
    // Asks the host for the plugin's state (stateChanged follows if it
    // changed), now or once its node is up.
    void requestState(const std::string &node);

Q_SIGNALS:
    // A host started, stopped, crashed or failed: errors may have changed,
    // and its node may have come or gone.
    void changed();
    // A control changed in the plugin's window. type: the plugin's (the
    // effect at the node's place may have changed since).
    void paramChanged(const std::string &node, const std::string &type, const std::string &name,
                      double value);
    // The plugin's own state changed (see PluginSpec::state).
    void stateChanged(const std::string &node, const std::string &type, const std::string &state);

private:
    using Clock = std::chrono::steady_clock;
    struct Host
    {
        PluginSpec spec;
        QProcess *process = nullptr;
        QTimer *restart = nullptr;
        QTimer *readyCheck = nullptr;
        bool ready = false; // its node is up
        QByteArray pending; // stdout not yet split into lines
        std::vector<Clock::time_point> crashes;
        std::string error;
        std::string reported; // the last error line the host printed
        bool metering = false;
        bool windowOpen = false;
        std::string windowError;
        std::optional<bool> windowWanted; // asked while it wasn't up
        bool stateWanted = false;
        bool hasLevels = false;
        EffectLevels levels;
    };

    void start(const std::string &node, Host &host);
    // Lets the host go: closes its stdin and waits a moment for it to exit
    // (taking in the state it sends; reported when report is set), then
    // kills it.
    void stop(const std::string &node, Host &host, bool report);
    void finished(const std::string &node, int exitCode, QProcess::ExitStatus status);
    void crashed(const std::string &node, Host &host, const std::string &why);
    void readOutput(const std::string &node);
    void handleLine(const std::string &node, Host &host, const QByteArray &bytes, bool report);
    static void send(Host &host, const QByteArray &message);

    std::map<std::string, std::unique_ptr<Host>> m_hosts;
};

} // namespace pp

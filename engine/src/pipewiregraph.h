// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "graphtypes.h"

#include <QObject>
#include <QTimer>

#include <map>
#include <memory>
#include <string>

namespace pp {

// A live mirror of the PipeWire graph: nodes, devices, ports, links and the
// "default" metadata.
//
// PipeWire runs on its own pw_thread_loop. Every event is copied and posted to
// the thread this object lives on (the Qt main thread), so the accessors and
// signals below are plain main-thread Qt. Nodes and devices are announced once
// their full info props have arrived, not with the reduced registry props.
//
// When the server goes away (for example `systemctl --user restart pipewire`)
// every object is reported removed and the graph reconnects on its own.
class PipeWireGraph : public QObject
{
    Q_OBJECT

public:
    explicit PipeWireGraph(QObject *parent = nullptr);
    ~PipeWireGraph() override;
    PipeWireGraph(const PipeWireGraph &) = delete;
    PipeWireGraph &operator=(const PipeWireGraph &) = delete;
    PipeWireGraph(PipeWireGraph &&) = delete;
    PipeWireGraph &operator=(PipeWireGraph &&) = delete;

    // Connects, and keeps retrying every reconnectInterval() until it succeeds.
    void start();
    void stop();

    [[nodiscard]] bool isConnected() const { return m_connected; }

    [[nodiscard]] const std::map<std::uint32_t, GraphObject> &nodes() const { return m_nodes; }
    [[nodiscard]] const std::map<std::uint32_t, GraphObject> &devices() const { return m_devices; }
    [[nodiscard]] const std::map<std::uint32_t, GraphObject> &ports() const { return m_ports; }
    [[nodiscard]] const std::map<std::uint32_t, GraphObject> &links() const { return m_links; }

    // node.name of the configured default sink and source, from the "default"
    // metadata. Empty when none is set.
    [[nodiscard]] const std::string &defaultSinkName() const { return m_defaultSink; }
    [[nodiscard]] const std::string &defaultSourceName() const { return m_defaultSource; }

    static constexpr int reconnectInterval() { return 2000; }

Q_SIGNALS:
    void connectedChanged(bool connected);
    void objectAdded(const pp::GraphObject &object);
    void objectChanged(const pp::GraphObject &object);
    // The object as it was last seen, so receivers can read its props.
    void objectRemoved(const pp::GraphObject &object);
    void defaultsChanged();

public:
    // One connection's PipeWire state; defined in the .cpp. Public only so the
    // PipeWire event tables there can name its callbacks.
    struct Session;

private:
    friend struct Session;

    void connectNow();
    void disconnectNow();

    // Called on the main thread via queued invocations from the PipeWire thread.
    // Events from an older session (before a reconnect) are dropped.
    void handleAdded(int generation, GraphObject object);
    void handleChanged(int generation, GraphObject object);
    void handleRemoved(int generation, std::uint32_t id);
    void handleMetadata(int generation, std::uint32_t subject, const std::string &key,
                        const std::string &value);
    void handleConnectionLost(int generation, const std::string &reason);

    std::map<std::uint32_t, GraphObject> *mapFor(const std::string &type);

    std::unique_ptr<Session> m_session;
    QTimer m_reconnectTimer;
    int m_generation = 0;
    bool m_connected = false;
    bool m_running = false;

    std::map<std::uint32_t, GraphObject> m_nodes;
    std::map<std::uint32_t, GraphObject> m_devices;
    std::map<std::uint32_t, GraphObject> m_ports;
    std::map<std::uint32_t, GraphObject> m_links;
    std::string m_defaultSink;
    std::string m_defaultSource;
};

} // namespace pp

Q_DECLARE_METATYPE(pp::GraphObject)

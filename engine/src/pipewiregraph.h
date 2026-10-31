// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "effects.h"
#include "endpoint.h"
#include "graphtypes.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

// Peak levels written by a meter stream on the PipeWire data thread and read
// (and reset) on the main thread. Linear amplitude, 1.0 = 0 dBFS.
struct MeterLevels
{
    explicit MeterLevels(std::size_t channels) : peaks(channels), rms(channels) {}
    std::vector<std::atomic<float>> peaks;
    // RMS per channel now (linear, ~300 ms), and the short-term loudness
    // (K-weighted over ~3 s, LUFS) of all channels together.
    std::vector<std::atomic<float>> rms;
    std::atomic<float> loudness{-70.0F};
    // The stream's sample rate once negotiated (0 until then).
    std::atomic<int> rate{0};

    // Returns the peaks since the last call and resets them.
    std::vector<float> take();
    // The RMS values now.
    [[nodiscard]] std::vector<float> rmsNow() const;
};

// What a stream added with PipeWireGraph::addStream() runs on the realtime
// thread: a recorder takes the samples, a player fills them. Samples are
// 32-bit float, interleaved.
class StreamIo
{
public:
    StreamIo() = default;
    virtual ~StreamIo() = default;
    StreamIo(const StreamIo &) = delete;
    StreamIo &operator=(const StreamIo &) = delete;
    StreamIo(StreamIo &&) = delete;
    StreamIo &operator=(StreamIo &&) = delete;

    // The stream's sample rate, once negotiated.
    virtual void setRate(int rate) noexcept = 0;
    // Capture: frames * channels samples to take. Playback: as many to fill.
    virtual void process(float *samples, std::uint32_t frames, std::uint32_t channels) noexcept = 0;
};

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

    // A stream's "target.object" in the "default" metadata: the object.serial
    // (or node.name) of the node it should link to, "-1" for the system
    // default, or empty when none is set. This is what WirePlumber follows and
    // what Plasma and pavucontrol write when an app is moved to another device.
    [[nodiscard]] std::string streamTarget(std::uint32_t nodeId) const;

    static constexpr int reconnectInterval() { return 2000; }

    // Commands. Each returns false when not connected or when PipeWire refuses
    // the request outright; the result shows up later as graph events.

    // Creates an object through a server-side factory, e.g. "adapter" for a
    // null-sink node or "link-factory" for a link. Set object.linger=true in
    // props to keep it after this client disconnects.
    bool createObject(const std::string &factory, const std::string &type, std::uint32_t version,
                      const Properties &props);
    bool createNode(const Properties &props);
    bool createLink(std::uint32_t outputNode, std::uint32_t outputPort, std::uint32_t inputNode,
                    std::uint32_t inputPort, bool linger);
    // Asks the server to destroy any object (needs permission on it).
    bool destroyObject(std::uint32_t id);
    // Sets the node's Props: one linear volume per channel, and mute.
    bool setNodeVolume(std::uint32_t nodeId, const std::vector<float> &channelVolumes, bool mute);

    // Makes nodeName the configured default sink or source, as `wpctl
    // set-default` does (writes default.configured.audio.sink/source in the
    // "default" metadata). WirePlumber then updates the effective default.
    bool setConfiguredDefault(Direction direction, const std::string &nodeName);

    // Sets a stream's target.object (see streamTarget()); an empty value clears it.
    bool setStreamTarget(std::uint32_t nodeId, const std::string &value);

    // PipeWire's runtime settings (the "settings" metadata): clock.rate,
    // clock.allowed-rates, clock.force-quantum and so on. setting() is empty
    // until the value is known.
    [[nodiscard]] std::string setting(const std::string &key) const;
    bool setSetting(const std::string &key, const std::string &value);

    // Starts a small capture stream on targetNodeName (the monitor of a sink,
    // or a source itself) and returns its peak levels. Meters belong to the
    // current connection: after a reconnect they must be added again.
    std::shared_ptr<MeterLevels> addMeter(const std::string &targetNodeName, int channels,
                                          bool sinkMonitor = true);
    void removeMeter(const std::shared_ptr<MeterLevels> &meter);

    // A processing node running processor on the realtime thread: a filter
    // named name with an input and an output port per channel position
    // ("input_FL", "output_FL"), and with keyInput one more mono input,
    // "input_KEY" (audio.channel "KEY"), fed to the processor as its key.
    // Like meters, filters belong to the current connection and to this
    // process: they go when either does. extraProps are added to the node
    // (description, owner tag, ...).
    bool addFilter(const std::string &name, const std::vector<std::string> &positions,
                   const Properties &extraProps, const std::shared_ptr<Processor> &processor,
                   bool keyInput = false);
    void removeFilter(const std::string &name);
    [[nodiscard]] bool hasFilter(const std::string &name) const;

    // A node named name that never links itself (the caller links it),
    // running io on the realtime thread, which always sees stereo: capture
    // (a recorder: a filter with an input port per position, "FL" "FR" or
    // just "MONO", heard in both channels; at the graph's rate) or playback
    // (a player: a stereo stream at rate, which PipeWire converts). Like
    // filters, they belong to the current connection.
    bool addStream(const std::string &name, bool capture, int rate, const Properties &extraProps,
                   const std::shared_ptr<StreamIo> &io,
                   const std::vector<std::string> &positions = {"FL", "FR"});
    void removeStream(const std::string &name);

Q_SIGNALS:
    void connectedChanged(bool connected);
    void objectAdded(const pp::GraphObject &object);
    void objectChanged(const pp::GraphObject &object);
    // The object as it was last seen, so receivers can read its props.
    void objectRemoved(const pp::GraphObject &object);
    void defaultsChanged();
    void streamTargetChanged(std::uint32_t nodeId);
    // A value in PipeWire's "settings" metadata changed (see setting()).
    void settingsChanged();

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
    void handleSetting(int generation, const std::string &key, const std::string &value);
    void handleConnectionLost(int generation, const std::string &reason);
    void handleReady(int generation);

    std::map<std::uint32_t, GraphObject> *mapFor(const std::string &type);
    bool setMetadata(std::uint32_t subject, const char *key, const char *type, const char *value,
                     std::string_view metadataName = "default");

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
    std::map<std::uint32_t, std::string> m_streamTargets;
    std::map<std::string, std::string, std::less<>> m_settings;
};

} // namespace pp

Q_DECLARE_METATYPE(pp::GraphObject)

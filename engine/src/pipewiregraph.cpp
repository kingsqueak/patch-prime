// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "pipewiregraph.h"

#include "graphsnapshot.h"

#include <QLoggingCategory>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string_view>

Q_LOGGING_CATEGORY(lcGraph, "patchprime.graph")

namespace pp {

namespace {

Properties toProperties(const spa_dict *dict)
{
    Properties out;
    if (dict == nullptr) {
        return out;
    }
    const spa_dict_item *item = nullptr;
    spa_dict_for_each(item, dict)
    {
        out.emplace(item->key, item->value != nullptr ? item->value : "");
    }
    return out;
}

pw_properties *toPwProperties(const Properties &props)
{
    pw_properties *out = pw_properties_new(nullptr, nullptr);
    for (const auto &[key, value] : props) {
        pw_properties_set(out, key.c_str(), value.c_str());
    }
    return out;
}

// Lock-free "store the larger value", for peaks written on the data thread.
void atomicMax(std::atomic<float> &target, float value)
{
    float current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

std::vector<float> MeterLevels::take()
{
    std::vector<float> out;
    out.reserve(peaks.size());
    for (auto &peak : peaks) {
        out.push_back(peak.exchange(0.0F, std::memory_order_relaxed));
    }
    return out;
}

// Everything owned by one connection to the server. All members except owner
// and generation are touched only on the PipeWire thread, or on the main thread
// with the thread loop locked (open and the destructor).
struct PipeWireGraph::Session
{
    // A proxy bound to follow an object's info or metadata. The object is
    // announced to the main thread on its first info event.
    struct Bound
    {
        Session *session = nullptr;
        GraphObject object;
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
        bool announced = false;
    };

    Session(PipeWireGraph *graphOwner, int sessionGeneration)
        : owner(graphOwner), generation(sessionGeneration)
    {
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&) = delete;
    Session &operator=(Session &&) = delete;
    ~Session();

    std::string open();

    template <typename F> void post(F &&function)
    {
        QMetaObject::invokeMethod(owner, std::forward<F>(function), Qt::QueuedConnection);
    }

    // A proxy for an object we asked a factory to create. It's kept until the
    // server removes the object or the session ends.
    struct Created
    {
        Session *session = nullptr;
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
    };

    // A capture stream measuring peaks on a sink's monitor.
    struct Meter
    {
        pw_stream *stream = nullptr;
        spa_hook listener{};
        std::shared_ptr<MeterLevels> levels;
    };

    void bind(const GraphObject &object, std::uint32_t version);

    static void onCoreError(void *data, std::uint32_t id, int seq, int res, const char *message);
    static void onGlobal(void *data, std::uint32_t id, std::uint32_t permissions, const char *type,
                         std::uint32_t version, const spa_dict *props);
    static void onGlobalRemove(void *data, std::uint32_t id);
    static void onNodeInfo(void *data, const pw_node_info *info);
    static void onDeviceInfo(void *data, const pw_device_info *info);
    static int onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                  const char *type, const char *value);
    static void onCreatedRemoved(void *data);
    static void onCreatedDestroy(void *data);
    static void onMeterProcess(void *data);

    void announce(Bound &bound, const spa_dict *props, bool propsChanged);

    PipeWireGraph *owner;
    int generation;

    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook coreListener{};
    spa_hook registryListener{};
    bool lost = false;

    std::map<std::uint32_t, std::unique_ptr<Bound>> bound; // nodes, devices, metadata
    std::map<std::uint32_t, bool> plain;                   // ports and links, announced at once
    std::map<pw_proxy *, std::unique_ptr<Created>> created;
    std::map<const MeterLevels *, std::unique_ptr<Meter>> meters;
};

namespace {

// Built field by field: see the note in graphsnapshot.cpp.
const pw_core_events coreEvents = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.error = &PipeWireGraph::Session::onCoreError;
    return e;
}();

const pw_registry_events registryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = &PipeWireGraph::Session::onGlobal;
    e.global_remove = &PipeWireGraph::Session::onGlobalRemove;
    return e;
}();

const pw_node_events nodeEvents = [] {
    pw_node_events e{};
    e.version = PW_VERSION_NODE_EVENTS;
    e.info = &PipeWireGraph::Session::onNodeInfo;
    return e;
}();

const pw_device_events deviceEvents = [] {
    pw_device_events e{};
    e.version = PW_VERSION_DEVICE_EVENTS;
    e.info = &PipeWireGraph::Session::onDeviceInfo;
    return e;
}();

const pw_metadata_events metadataEvents = [] {
    pw_metadata_events e{};
    e.version = PW_VERSION_METADATA_EVENTS;
    e.property = &PipeWireGraph::Session::onMetadataProperty;
    return e;
}();

const pw_proxy_events createdEvents = [] {
    pw_proxy_events e{};
    e.version = PW_VERSION_PROXY_EVENTS;
    e.removed = &PipeWireGraph::Session::onCreatedRemoved;
    e.destroy = &PipeWireGraph::Session::onCreatedDestroy;
    return e;
}();

const pw_stream_events meterEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = &PipeWireGraph::Session::onMeterProcess;
    return e;
}();

} // namespace

std::string PipeWireGraph::Session::open()
{
    loop = pw_thread_loop_new("patchprime-pw", nullptr);
    if (loop == nullptr) {
        return "could not create a PipeWire thread loop";
    }
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    if (context == nullptr) {
        return "could not create a PipeWire context";
    }
    if (pw_thread_loop_start(loop) < 0) {
        return "could not start the PipeWire thread";
    }

    pw_thread_loop_lock(loop);
    core = pw_context_connect(context, nullptr, 0);
    if (core == nullptr) {
        const int err = errno;
        pw_thread_loop_unlock(loop);
        return std::string("could not connect to PipeWire: ") + std::strerror(err);
    }
    pw_core_add_listener(core, &coreListener, &coreEvents, this);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
    pw_thread_loop_unlock(loop);
    return {};
}

PipeWireGraph::Session::~Session()
{
    if (loop == nullptr) {
        return;
    }
    pw_thread_loop_lock(loop);
    for (auto &[levels, meter] : meters) {
        spa_hook_remove(&meter->listener);
        pw_stream_destroy(meter->stream);
    }
    meters.clear();
    // Lingering objects (buses, links) stay on the server after this.
    for (auto &[proxy, c] : created) {
        spa_hook_remove(&c->listener);
        pw_proxy_destroy(proxy);
    }
    created.clear();
    for (auto &[id, b] : bound) {
        spa_hook_remove(&b->listener);
        pw_proxy_destroy(b->proxy);
    }
    bound.clear();
    if (registry != nullptr) {
        spa_hook_remove(&registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    }
    if (core != nullptr) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
    }
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
    if (context != nullptr) {
        pw_context_destroy(context);
    }
    pw_thread_loop_destroy(loop);
}

void PipeWireGraph::Session::onCoreError(void *data, std::uint32_t id, int /*seq*/, int res,
                                         const char *message)
{
    if (id != PW_ID_CORE) {
        return; // an error on one of our bound proxies, e.g. it was removed mid-bind
    }
    auto *self = static_cast<Session *>(data);
    if (self->lost) {
        return;
    }
    self->lost = true;
    std::string reason =
        std::string(message != nullptr ? message : "") + " (" + spa_strerror(res) + ")";
    self->post([owner = self->owner, generation = self->generation, reason = std::move(reason)] {
        owner->handleConnectionLost(generation, reason);
    });
}

void PipeWireGraph::Session::onGlobal(void *data, std::uint32_t id, std::uint32_t /*permissions*/,
                                      const char *type, std::uint32_t version,
                                      const spa_dict *props)
{
    auto *self = static_cast<Session *>(data);
    const std::string_view typeName = type;
    GraphObject object{.id = id, .type = type, .version = version, .props = toProperties(props)};

    if (typeName == PW_TYPE_INTERFACE_Node || typeName == PW_TYPE_INTERFACE_Device) {
        self->bind(object, version);
    } else if (typeName == PW_TYPE_INTERFACE_Metadata) {
        if (prop(object, PW_KEY_METADATA_NAME) == "default") {
            self->bind(object, version);
        }
    } else if (typeName == PW_TYPE_INTERFACE_Port || typeName == PW_TYPE_INTERFACE_Link) {
        self->plain.emplace(id, true);
        self->post([owner = self->owner, generation = self->generation,
                    object = std::move(object)]() mutable {
            owner->handleAdded(generation, std::move(object));
        });
    }
}

void PipeWireGraph::Session::bind(const GraphObject &object, std::uint32_t /*version*/)
{
    auto b = std::make_unique<Bound>();
    b->session = this;
    b->object = object;

    const std::string_view type = object.type;
    if (type == PW_TYPE_INTERFACE_Node) {
        auto *node = static_cast<pw_node *>(
            pw_registry_bind(registry, object.id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (node == nullptr) {
            return;
        }
        pw_node_add_listener(node, &b->listener, &nodeEvents, b.get());
        b->proxy = reinterpret_cast<pw_proxy *>(node);
    } else if (type == PW_TYPE_INTERFACE_Device) {
        auto *device = static_cast<pw_device *>(
            pw_registry_bind(registry, object.id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
        if (device == nullptr) {
            return;
        }
        pw_device_add_listener(device, &b->listener, &deviceEvents, b.get());
        b->proxy = reinterpret_cast<pw_proxy *>(device);
    } else {
        auto *metadata = static_cast<pw_metadata *>(pw_registry_bind(
            registry, object.id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
        if (metadata == nullptr) {
            return;
        }
        pw_metadata_add_listener(metadata, &b->listener, &metadataEvents, b.get());
        b->proxy = reinterpret_cast<pw_proxy *>(metadata);
    }
    bound[object.id] = std::move(b);
}

void PipeWireGraph::Session::onGlobalRemove(void *data, std::uint32_t id)
{
    auto *self = static_cast<Session *>(data);
    bool announced = false;
    if (auto it = self->bound.find(id); it != self->bound.end()) {
        announced = it->second->announced;
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(it->second->proxy);
        self->bound.erase(it);
    } else if (auto plainIt = self->plain.find(id); plainIt != self->plain.end()) {
        announced = true;
        self->plain.erase(plainIt);
    }
    if (announced) {
        self->post([owner = self->owner, generation = self->generation, id] {
            owner->handleRemoved(generation, id);
        });
    }
}

void PipeWireGraph::Session::announce(Bound &b, const spa_dict *props, bool propsChanged)
{
    if (propsChanged && props != nullptr) {
        b.object.props = toProperties(props);
    } else if (b.announced) {
        return; // only params or state changed; nothing we mirror
    }
    const bool first = !b.announced;
    b.announced = true;
    post([owner = owner, generation = generation, first, object = b.object]() mutable {
        if (first) {
            owner->handleAdded(generation, std::move(object));
        } else {
            owner->handleChanged(generation, std::move(object));
        }
    });
}

void PipeWireGraph::Session::onNodeInfo(void *data, const pw_node_info *info)
{
    auto *b = static_cast<Bound *>(data);
    b->session->announce(*b, info->props, (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0);
}

void PipeWireGraph::Session::onDeviceInfo(void *data, const pw_device_info *info)
{
    auto *b = static_cast<Bound *>(data);
    b->session->announce(*b, info->props, (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) != 0);
}

int PipeWireGraph::Session::onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                               const char * /*type*/, const char *value)
{
    auto *b = static_cast<Bound *>(data);
    // A null key clears every property; it's passed on as an empty key.
    b->session->post([owner = b->session->owner, generation = b->session->generation, subject,
                      key = std::string(key != nullptr ? key : ""),
                      value = std::string(value != nullptr ? value : "")] {
        owner->handleMetadata(generation, subject, key, value);
    });
    return 0;
}

void PipeWireGraph::Session::onCreatedRemoved(void *data)
{
    // The server removed the object; our proxy has to be destroyed too.
    pw_proxy_destroy(static_cast<Created *>(data)->proxy);
}

void PipeWireGraph::Session::onCreatedDestroy(void *data)
{
    auto *c = static_cast<Created *>(data);
    spa_hook_remove(&c->listener);
    c->session->created.erase(c->proxy); // frees c
}

// Realtime data thread: no allocation, no locks.
void PipeWireGraph::Session::onMeterProcess(void *data)
{
    auto *meter = static_cast<Meter *>(data);
    pw_buffer *buffer = pw_stream_dequeue_buffer(meter->stream);
    if (buffer == nullptr) {
        return;
    }
    const spa_data &d = buffer->buffer->datas[0];
    auto &peaks = meter->levels->peaks;
    if (d.data != nullptr && d.chunk != nullptr && !peaks.empty()) {
        const auto *bytes = static_cast<const std::byte *>(d.data) + d.chunk->offset;
        const auto *samples = reinterpret_cast<const float *>(bytes);
        const std::size_t count = d.chunk->size / sizeof(float);
        const std::size_t channels = peaks.size();
        std::array<float, 64> local{};
        const std::size_t tracked = std::min(channels, local.size());
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t channel = i % channels;
            // Bounds are checked by hand: .at() could throw on the realtime thread.
            if (channel < tracked) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                local[channel] = std::max(local[channel], std::fabs(samples[i]));
            }
        }
        for (std::size_t c = 0; c < tracked; ++c) {
            atomicMax(peaks[c],
                      local[c]); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        }
    }
    pw_stream_queue_buffer(meter->stream, buffer);
}

PipeWireGraph::PipeWireGraph(QObject *parent) : QObject(parent)
{
    static std::once_flag once;
    std::call_once(once, [] {
        pw_init(nullptr, nullptr);
        qRegisterMetaType<pp::GraphObject>();
    });
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(reconnectInterval());
    connect(&m_reconnectTimer, &QTimer::timeout, this, &PipeWireGraph::connectNow);
}

PipeWireGraph::~PipeWireGraph()
{
    // Stop the PipeWire thread without emitting signals into a half-destroyed object.
    m_session.reset();
}

void PipeWireGraph::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    connectNow();
}

void PipeWireGraph::stop()
{
    m_running = false;
    m_reconnectTimer.stop();
    disconnectNow();
}

void PipeWireGraph::connectNow()
{
    if (!m_running || m_session) {
        return;
    }
    ++m_generation;
    auto session = std::make_unique<Session>(this, m_generation);
    if (const auto error = session->open(); !error.empty()) {
        qCWarning(lcGraph, "%s; retrying in %d ms", error.c_str(), reconnectInterval());
        session.reset();
        m_reconnectTimer.start();
        return;
    }
    m_session = std::move(session);
    m_connected = true;
    Q_EMIT connectedChanged(true);
}

void PipeWireGraph::disconnectNow()
{
    m_session.reset();
    ++m_generation; // drop events still queued from the old session

    // Report everything gone, links first so receivers never see a link whose
    // ports have already disappeared.
    for (auto *map : {&m_links, &m_ports, &m_nodes, &m_devices}) {
        auto objects = std::move(*map);
        map->clear();
        for (const auto &[id, object] : objects) {
            Q_EMIT objectRemoved(object);
        }
    }
    if (!m_defaultSink.empty() || !m_defaultSource.empty()) {
        m_defaultSink.clear();
        m_defaultSource.clear();
        Q_EMIT defaultsChanged();
    }
    if (m_connected) {
        m_connected = false;
        Q_EMIT connectedChanged(false);
    }
}

bool PipeWireGraph::createObject(const std::string &factory, const std::string &type,
                                 std::uint32_t version, const Properties &props)
{
    if (!m_session) {
        return false;
    }
    auto &session = *m_session;
    pw_thread_loop_lock(session.loop);
    pw_properties *properties = toPwProperties(props);
    auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(
        session.core, factory.c_str(), type.c_str(), version, &properties->dict, 0));
    pw_properties_free(properties);
    if (proxy != nullptr) {
        auto c = std::make_unique<Session::Created>();
        c->session = &session;
        c->proxy = proxy;
        pw_proxy_add_listener(proxy, &c->listener, &createdEvents, c.get());
        session.created[proxy] = std::move(c);
    }
    pw_thread_loop_unlock(session.loop);
    return proxy != nullptr;
}

bool PipeWireGraph::createNode(const Properties &props)
{
    return createObject("adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, props);
}

bool PipeWireGraph::createLink(std::uint32_t outputNode, std::uint32_t outputPort,
                               std::uint32_t inputNode, std::uint32_t inputPort, bool linger)
{
    return createObject("link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                        {{PW_KEY_LINK_OUTPUT_NODE, std::to_string(outputNode)},
                         {PW_KEY_LINK_OUTPUT_PORT, std::to_string(outputPort)},
                         {PW_KEY_LINK_INPUT_NODE, std::to_string(inputNode)},
                         {PW_KEY_LINK_INPUT_PORT, std::to_string(inputPort)},
                         {PW_KEY_OBJECT_LINGER, linger ? "true" : "false"}});
}

bool PipeWireGraph::destroyObject(std::uint32_t id)
{
    if (!m_session) {
        return false;
    }
    pw_thread_loop_lock(m_session->loop);
    const int res = pw_registry_destroy(m_session->registry, id);
    pw_thread_loop_unlock(m_session->loop);
    return res >= 0;
}

bool PipeWireGraph::setNodeVolume(std::uint32_t nodeId, const std::vector<float> &channelVolumes,
                                  bool mute)
{
    if (!m_session || channelVolumes.empty()) {
        return false;
    }
    auto &session = *m_session;
    pw_thread_loop_lock(session.loop);
    bool ok = false;
    if (auto it = session.bound.find(nodeId);
        it != session.bound.end() && it->second->object.type == PW_TYPE_INTERFACE_Node) {
        std::array<std::uint8_t, 1024> buffer{};
        spa_pod_builder b{};
        spa_pod_builder_init(&b, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
        spa_pod_frame frame{};
        spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
                              static_cast<std::uint32_t>(channelVolumes.size()),
                              channelVolumes.data());
        spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&b, mute);
        auto *param = static_cast<spa_pod *>(spa_pod_builder_pop(&b, &frame));
        ok = pw_node_set_param(reinterpret_cast<pw_node *>(it->second->proxy), SPA_PARAM_Props, 0,
                               param) >= 0;
    }
    pw_thread_loop_unlock(session.loop);
    return ok;
}

std::shared_ptr<MeterLevels> PipeWireGraph::addMeter(const std::string &targetNodeName,
                                                     int channels)
{
    if (!m_session || channels <= 0) {
        return nullptr;
    }
    auto &session = *m_session;
    auto meter = std::make_unique<Session::Meter>();
    meter->levels = std::make_shared<MeterLevels>(static_cast<std::size_t>(channels));

    pw_properties *props = toPwProperties({
        {PW_KEY_MEDIA_TYPE, "Audio"},
        {PW_KEY_MEDIA_CATEGORY, "Monitor"},
        {PW_KEY_NODE_NAME, "patchprime.meter." + targetNodeName},
        {PW_KEY_TARGET_OBJECT, targetNodeName},
        {PW_KEY_STREAM_CAPTURE_SINK, "true"},
        {PW_KEY_NODE_DONT_RECONNECT, "true"},
        {PW_KEY_NODE_PASSIVE, "true"},
        {"stream.dont-remix", "true"},
        {"node.latency", "1024/48000"},
    });

    pw_thread_loop_lock(session.loop);
    meter->stream = pw_stream_new(session.core, "Patch Prime meter", props); // takes props
    bool ok = meter->stream != nullptr;
    if (ok) {
        pw_stream_add_listener(meter->stream, &meter->listener, &meterEvents, meter.get());
        std::array<std::uint8_t, 1024> buffer{};
        spa_pod_builder b{};
        spa_pod_builder_init(&b, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = static_cast<std::uint32_t>(channels);
        std::array<const spa_pod *, 1> params{
            spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        const auto flags = static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
        ok = pw_stream_connect(meter->stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params.data(),
                               static_cast<std::uint32_t>(params.size())) >= 0;
        if (!ok) {
            spa_hook_remove(&meter->listener);
            pw_stream_destroy(meter->stream);
        }
    }
    std::shared_ptr<MeterLevels> levels;
    if (ok) {
        levels = meter->levels;
        session.meters[levels.get()] = std::move(meter);
    }
    pw_thread_loop_unlock(session.loop);
    return levels;
}

void PipeWireGraph::removeMeter(const std::shared_ptr<MeterLevels> &meter)
{
    if (!m_session || !meter) {
        return;
    }
    auto &session = *m_session;
    pw_thread_loop_lock(session.loop);
    if (auto it = session.meters.find(meter.get()); it != session.meters.end()) {
        spa_hook_remove(&it->second->listener);
        pw_stream_destroy(it->second->stream);
        session.meters.erase(it);
    }
    pw_thread_loop_unlock(session.loop);
}

std::map<std::uint32_t, GraphObject> *PipeWireGraph::mapFor(const std::string &type)
{
    if (type == PW_TYPE_INTERFACE_Node) {
        return &m_nodes;
    }
    if (type == PW_TYPE_INTERFACE_Device) {
        return &m_devices;
    }
    if (type == PW_TYPE_INTERFACE_Port) {
        return &m_ports;
    }
    if (type == PW_TYPE_INTERFACE_Link) {
        return &m_links;
    }
    return nullptr;
}

void PipeWireGraph::handleAdded(int generation, GraphObject object)
{
    if (generation != m_generation) {
        return;
    }
    auto *map = mapFor(object.type);
    if (map == nullptr) {
        return;
    }
    const auto [it, inserted] = map->insert_or_assign(object.id, std::move(object));
    if (inserted) {
        Q_EMIT objectAdded(it->second);
    } else {
        Q_EMIT objectChanged(it->second);
    }
}

void PipeWireGraph::handleChanged(int generation, GraphObject object)
{
    if (generation != m_generation) {
        return;
    }
    auto *map = mapFor(object.type);
    if (map == nullptr) {
        return;
    }
    auto it = map->find(object.id);
    if (it == map->end()) {
        return;
    }
    it->second = std::move(object);
    Q_EMIT objectChanged(it->second);
}

void PipeWireGraph::handleRemoved(int generation, std::uint32_t id)
{
    if (generation != m_generation) {
        return;
    }
    for (auto *map : {&m_links, &m_ports, &m_nodes, &m_devices}) {
        if (auto it = map->find(id); it != map->end()) {
            const GraphObject object = std::move(it->second);
            map->erase(it);
            Q_EMIT objectRemoved(object);
            return;
        }
    }
}

void PipeWireGraph::handleMetadata(int generation, std::uint32_t subject, const std::string &key,
                                   const std::string &value)
{
    if (generation != m_generation || subject != PW_ID_CORE) {
        return;
    }
    const auto sink = m_defaultSink;
    const auto source = m_defaultSource;
    if (key.empty()) {
        m_defaultSink.clear();
        m_defaultSource.clear();
    } else if (key == "default.audio.sink") {
        m_defaultSink = defaultNodeName(value);
    } else if (key == "default.audio.source") {
        m_defaultSource = defaultNodeName(value);
    }
    if (sink != m_defaultSink || source != m_defaultSource) {
        Q_EMIT defaultsChanged();
    }
}

void PipeWireGraph::handleConnectionLost(int generation, const std::string &reason)
{
    if (generation != m_generation) {
        return;
    }
    qCWarning(lcGraph, "lost the PipeWire connection: %s; reconnecting", reason.c_str());
    disconnectNow();
    if (m_running) {
        m_reconnectTimer.start();
    }
}

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "pipewiregraph.h"

#include "graphsnapshot.h"

#include <QLoggingCategory>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include <cerrno>
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

} // namespace

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

    void bind(const GraphObject &object, std::uint32_t version);
    void unbind(std::unique_ptr<Bound> bound);

    static void onCoreError(void *data, std::uint32_t id, int seq, int res, const char *message);
    static void onGlobal(void *data, std::uint32_t id, std::uint32_t permissions, const char *type,
                         std::uint32_t version, const spa_dict *props);
    static void onGlobalRemove(void *data, std::uint32_t id);
    static void onNodeInfo(void *data, const pw_node_info *info);
    static void onDeviceInfo(void *data, const pw_device_info *info);
    static int onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                  const char *type, const char *value);

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

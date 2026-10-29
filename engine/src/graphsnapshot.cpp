// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "graphsnapshot.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>

namespace pp {

namespace {

void ensurePipeWireInitialised()
{
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

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

// Runs one pw_main_loop until two core round trips have completed: the first
// returns every registry global, the second returns the info and metadata
// events for the objects bound while handling the first.
class Collector
{
public:
    explicit Collector(GraphSnapshot &snapshot) : m_snapshot(snapshot) {}
    Collector(const Collector &) = delete;
    Collector &operator=(const Collector &) = delete;
    Collector(Collector &&) = delete;
    Collector &operator=(Collector &&) = delete;
    ~Collector() { teardown(); }

    std::string run(std::chrono::milliseconds timeout);

private:
    enum class Kind : std::uint8_t
    {
        Node,
        Device,
        Metadata,
    };

    // A proxy bound to read full object info. Its index points into the
    // snapshot vector for its kind, which keeps growing while we collect.
    struct Bound
    {
        Collector *self = nullptr;
        Kind kind = Kind::Node;
        std::size_t index = 0;
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
    };

public:
    // PipeWire event callbacks. Public only so the event tables below can name them.
    static void onCoreInfo(void *data, const pw_core_info *info);
    static void onCoreDone(void *data, std::uint32_t id, int seq);
    static void onCoreError(void *data, std::uint32_t id, int seq, int res, const char *message);
    static void onRegistryGlobal(void *data, std::uint32_t id, std::uint32_t permissions,
                                 const char *type, std::uint32_t version, const spa_dict *props);
    static void onNodeInfo(void *data, const pw_node_info *info);
    static void onDeviceInfo(void *data, const pw_device_info *info);
    static int onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                  const char *type, const char *value);
    static void onTimeout(void *data, std::uint64_t expirations);

private:
    void bind(std::uint32_t id, Kind kind, std::size_t index);
    void fail(std::string message);
    void teardown();

    GraphSnapshot &m_snapshot;
    std::string m_error;

    pw_main_loop *m_loop = nullptr;
    pw_context *m_context = nullptr;
    pw_core *m_core = nullptr;
    pw_registry *m_registry = nullptr;
    spa_source *m_timer = nullptr;
    spa_hook m_coreListener{};
    spa_hook m_registryListener{};
    std::vector<std::unique_ptr<Bound>> m_bound;

    int m_pendingSeq = 0;
    int m_roundTrips = 0;
};

// Built field by field: the PipeWire event structs gain members over time and
// -Wmissing-field-initializers flags any designated initializer that skips one.
const pw_core_events coreEvents = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.info = &Collector::onCoreInfo;
    e.done = &Collector::onCoreDone;
    e.error = &Collector::onCoreError;
    return e;
}();

const pw_registry_events registryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = &Collector::onRegistryGlobal;
    return e;
}();

const pw_node_events nodeEvents = [] {
    pw_node_events e{};
    e.version = PW_VERSION_NODE_EVENTS;
    e.info = &Collector::onNodeInfo;
    return e;
}();

const pw_device_events deviceEvents = [] {
    pw_device_events e{};
    e.version = PW_VERSION_DEVICE_EVENTS;
    e.info = &Collector::onDeviceInfo;
    return e;
}();

const pw_metadata_events metadataEvents = [] {
    pw_metadata_events e{};
    e.version = PW_VERSION_METADATA_EVENTS;
    e.property = &Collector::onMetadataProperty;
    return e;
}();

std::string Collector::run(std::chrono::milliseconds timeout)
{
    m_loop = pw_main_loop_new(nullptr);
    if (m_loop == nullptr) {
        return "could not create a PipeWire main loop";
    }
    pw_loop *loop = pw_main_loop_get_loop(m_loop);

    m_context = pw_context_new(loop, nullptr, 0);
    if (m_context == nullptr) {
        return "could not create a PipeWire context";
    }

    m_core = pw_context_connect(m_context, nullptr, 0);
    if (m_core == nullptr) {
        return std::string("could not connect to PipeWire: ") + std::strerror(errno);
    }
    pw_core_add_listener(m_core, &m_coreListener, &coreEvents, this);

    m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(m_registry, &m_registryListener, &registryEvents, this);

    m_timer = pw_loop_add_timer(loop, &Collector::onTimeout, this);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    timespec value{};
    value.tv_sec = static_cast<time_t>(seconds.count());
    value.tv_nsec = static_cast<long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - seconds).count());
    pw_loop_update_timer(loop, m_timer, &value, nullptr, false);

    m_pendingSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
    pw_main_loop_run(m_loop);

    m_snapshot.server.libraryVersion = pw_get_library_version();
    return m_error;
}

void Collector::onCoreInfo(void *data, const pw_core_info *info)
{
    auto *self = static_cast<Collector *>(data);
    if (info->name != nullptr) {
        self->m_snapshot.server.name = info->name;
    }
    if (info->version != nullptr) {
        self->m_snapshot.server.version = info->version;
    }
}

void Collector::onCoreDone(void *data, std::uint32_t id, int seq)
{
    auto *self = static_cast<Collector *>(data);
    if (id != PW_ID_CORE || seq != self->m_pendingSeq) {
        return;
    }
    if (++self->m_roundTrips < 2) {
        self->m_pendingSeq = pw_core_sync(self->m_core, PW_ID_CORE, self->m_pendingSeq);
        return;
    }
    pw_main_loop_quit(self->m_loop);
}

void Collector::onCoreError(void *data, std::uint32_t id, int /*seq*/, int res, const char *message)
{
    // Errors on our bound proxies (for example an object removed before its
    // info arrived) don't spoil the snapshot. Only an error on the core does.
    if (id != PW_ID_CORE) {
        return;
    }
    auto *self = static_cast<Collector *>(data);
    self->fail(std::string("PipeWire error: ") + (message != nullptr ? message : "") + " (" +
               spa_strerror(res) + ")");
}

void Collector::onRegistryGlobal(void *data, std::uint32_t id, std::uint32_t /*permissions*/,
                                 const char *type, std::uint32_t version, const spa_dict *props)
{
    auto *self = static_cast<Collector *>(data);
    auto &snapshot = self->m_snapshot;
    const std::string_view typeName = type;

    GraphObject object{.id = id, .type = type, .version = version, .props = toProperties(props)};

    if (typeName == PW_TYPE_INTERFACE_Node) {
        snapshot.nodes.push_back(std::move(object));
        self->bind(id, Kind::Node, snapshot.nodes.size() - 1);
    } else if (typeName == PW_TYPE_INTERFACE_Device) {
        snapshot.devices.push_back(std::move(object));
        self->bind(id, Kind::Device, snapshot.devices.size() - 1);
    } else if (typeName == PW_TYPE_INTERFACE_Port) {
        snapshot.ports.push_back(std::move(object));
    } else if (typeName == PW_TYPE_INTERFACE_Link) {
        snapshot.links.push_back(std::move(object));
    } else if (typeName == PW_TYPE_INTERFACE_Metadata) {
        const char *name =
            props != nullptr ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
        if (name != nullptr && std::string_view(name) == "default") {
            self->bind(id, Kind::Metadata, 0);
        }
    }
}

void Collector::bind(std::uint32_t id, Kind kind, std::size_t index)
{
    auto bound = std::make_unique<Bound>();
    bound->self = this;
    bound->kind = kind;
    bound->index = index;

    switch (kind) {
    case Kind::Node: {
        auto *node = static_cast<pw_node *>(
            pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (node == nullptr) {
            return;
        }
        pw_node_add_listener(node, &bound->listener, &nodeEvents, bound.get());
        bound->proxy = reinterpret_cast<pw_proxy *>(node);
        break;
    }
    case Kind::Device: {
        auto *device = static_cast<pw_device *>(
            pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
        if (device == nullptr) {
            return;
        }
        pw_device_add_listener(device, &bound->listener, &deviceEvents, bound.get());
        bound->proxy = reinterpret_cast<pw_proxy *>(device);
        break;
    }
    case Kind::Metadata: {
        auto *metadata = static_cast<pw_metadata *>(
            pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
        if (metadata == nullptr) {
            return;
        }
        pw_metadata_add_listener(metadata, &bound->listener, &metadataEvents, bound.get());
        bound->proxy = reinterpret_cast<pw_proxy *>(metadata);
        break;
    }
    }
    m_bound.push_back(std::move(bound));
}

void Collector::onNodeInfo(void *data, const pw_node_info *info)
{
    auto *bound = static_cast<Bound *>(data);
    if (info->props != nullptr) {
        bound->self->m_snapshot.nodes[bound->index].props = toProperties(info->props);
    }
}

void Collector::onDeviceInfo(void *data, const pw_device_info *info)
{
    auto *bound = static_cast<Bound *>(data);
    if (info->props != nullptr) {
        bound->self->m_snapshot.devices[bound->index].props = toProperties(info->props);
    }
}

int Collector::onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                  const char *type, const char *value)
{
    auto *bound = static_cast<Bound *>(data);
    // A null key means "all properties cleared", a null value means "removed".
    // Neither happens in the initial burst a fresh bind receives.
    if (key != nullptr && value != nullptr) {
        bound->self->m_snapshot.defaults.push_back(
            {.subject = subject, .key = key, .type = type != nullptr ? type : "", .value = value});
    }
    return 0;
}

void Collector::onTimeout(void *data, std::uint64_t /*expirations*/)
{
    static_cast<Collector *>(data)->fail("timed out waiting for PipeWire");
}

void Collector::fail(std::string message)
{
    if (m_error.empty()) {
        m_error = std::move(message);
    }
    pw_main_loop_quit(m_loop);
}

void Collector::teardown()
{
    for (auto &bound : m_bound) {
        spa_hook_remove(&bound->listener);
        pw_proxy_destroy(bound->proxy);
    }
    m_bound.clear();
    if (m_registry != nullptr) {
        spa_hook_remove(&m_registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
        m_registry = nullptr;
    }
    if (m_core != nullptr) {
        spa_hook_remove(&m_coreListener);
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    if (m_timer != nullptr) {
        pw_loop_destroy_source(pw_main_loop_get_loop(m_loop), m_timer);
        m_timer = nullptr;
    }
    if (m_context != nullptr) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    if (m_loop != nullptr) {
        pw_main_loop_destroy(m_loop);
        m_loop = nullptr;
    }
}

} // namespace

std::expected<GraphSnapshot, std::string> takeGraphSnapshot(const SnapshotOptions &options)
{
    ensurePipeWireInitialised();
    GraphSnapshot snapshot;
    std::string error;
    {
        Collector collector(snapshot);
        error = collector.run(options.timeout);
    }
    if (!error.empty()) {
        return std::unexpected(std::move(error));
    }
    return snapshot;
}

std::string defaultNodeName(const std::string &jsonValue)
{
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(jsonValue));
    if (!document.isObject()) {
        return {};
    }
    return document.object().value(QLatin1String("name")).toString().toStdString();
}

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "alsamidi.h"

#include <QSocketNotifier>

#include <alsa/asoundlib.h>

#include <array>
#include <cerrno>
#include <print>
#include <utility>

namespace pp {

namespace {

std::string portId(std::string client, const std::string &port)
{
    client += ':';
    client += port;
    return client;
}

std::string trimmed(const char *text)
{
    std::string s = text != nullptr ? text : "";
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

template <typename T, void (*Free)(T *)>
struct Freer
{
    void operator()(T *p) const { Free(p); }
};
using ClientInfo = std::unique_ptr<snd_seq_client_info_t,
                                   Freer<snd_seq_client_info_t, snd_seq_client_info_free>>;
using PortInfo =
    std::unique_ptr<snd_seq_port_info_t, Freer<snd_seq_port_info_t, snd_seq_port_info_free>>;

bool isAnnouncement(unsigned type)
{
    switch (type) {
    case SND_SEQ_EVENT_CLIENT_START:
    case SND_SEQ_EVENT_CLIENT_EXIT:
    case SND_SEQ_EVENT_CLIENT_CHANGE:
    case SND_SEQ_EVENT_PORT_START:
    case SND_SEQ_EVENT_PORT_EXIT:
    case SND_SEQ_EVENT_PORT_CHANGE:
        return true;
    default:
        return false;
    }
}

} // namespace

struct AlsaMidi::Impl
{
    snd_seq_t *seq = nullptr;
    int client = -1;
    int port = -1;
    snd_midi_event_t *decoder = nullptr;
    snd_midi_event_t *encoder = nullptr;
    // The controller's port while connected.
    int targetClient = -1;
    int targetPort = -1;

    Impl() = default;
    ~Impl()
    {
        if (decoder != nullptr) {
            snd_midi_event_free(decoder);
        }
        if (encoder != nullptr) {
            snd_midi_event_free(encoder);
        }
        if (seq != nullptr) {
            snd_seq_close(seq); // drops our connections too
        }
    }
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    // The port called id ("<client>:<port>"), or {-1, -1}.
    [[nodiscard]] std::pair<int, int> find(const std::string &id) const;
};

std::pair<int, int> AlsaMidi::Impl::find(const std::string &id) const
{
    snd_seq_client_info_t *c = nullptr;
    snd_seq_port_info_t *p = nullptr;
    snd_seq_client_info_malloc(&c);
    snd_seq_port_info_malloc(&p);
    const ClientInfo clientInfo(c);
    const PortInfo portInfo(p);
    snd_seq_client_info_set_client(c, -1);
    while (snd_seq_query_next_client(seq, c) >= 0) {
        const int number = snd_seq_client_info_get_client(c);
        const auto clientName = trimmed(snd_seq_client_info_get_name(c));
        snd_seq_port_info_set_client(p, number);
        snd_seq_port_info_set_port(p, -1);
        while (snd_seq_query_next_port(seq, p) >= 0) {
            if (portId(clientName, trimmed(snd_seq_port_info_get_name(p))) == id) {
                return {number, snd_seq_port_info_get_port(p)};
            }
        }
    }
    return {-1, -1};
}

AlsaMidi::AlsaMidi(QObject *parent)
    : MidiPort(parent)
    , d(std::make_unique<Impl>())
{
    snd_seq_t *seq = nullptr;
    if (const int error = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
        error < 0) {
        std::println(stderr, "patchprimed: no ALSA sequencer, so no control surface ({})",
                     snd_strerror(error));
        return;
    }
    d->seq = seq;
    snd_seq_set_client_name(seq, "Patch Prime");
    d->port = snd_seq_create_simple_port(
        seq, "Control Surface",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
            SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (d->port < 0) {
        std::println(stderr, "patchprimed: can't make an ALSA MIDI port ({})", snd_strerror(d->port));
        d = std::make_unique<Impl>();
        return;
    }
    d->client = snd_seq_client_id(seq);
    // Ports coming and going arrive on our port as announcements.
    snd_seq_connect_from(seq, d->port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    constexpr std::size_t codecBuffer = 1024;
    snd_midi_event_new(codecBuffer, &d->decoder);
    snd_midi_event_new(codecBuffer, &d->encoder);
    snd_midi_event_no_status(d->decoder, 1); // every message with its status byte

    const int count = snd_seq_poll_descriptors_count(seq, POLLIN);
    std::vector<pollfd> fds(static_cast<std::size_t>(std::max(count, 0)));
    snd_seq_poll_descriptors(seq, fds.data(), static_cast<unsigned>(fds.size()), POLLIN);
    for (const auto &fd : fds) {
        auto notifier = std::make_unique<QSocketNotifier>(fd.fd, QSocketNotifier::Read);
        connect(notifier.get(), &QSocketNotifier::activated, this, &AlsaMidi::readEvents);
        m_notifiers.push_back(std::move(notifier));
    }
}

AlsaMidi::~AlsaMidi() = default;

bool AlsaMidi::isAvailable() const
{
    return d->seq != nullptr;
}

int AlsaMidi::clientId() const
{
    return d->client;
}

std::vector<AlsaMidi::Port> AlsaMidi::ports() const
{
    std::vector<Port> out;
    if (d->seq == nullptr) {
        return out;
    }
    snd_seq_client_info_t *c = nullptr;
    snd_seq_port_info_t *p = nullptr;
    snd_seq_client_info_malloc(&c);
    snd_seq_port_info_malloc(&p);
    const ClientInfo clientInfo(c);
    const PortInfo portInfo(p);
    snd_seq_client_info_set_client(c, -1);
    while (snd_seq_query_next_client(d->seq, c) >= 0) {
        const int number = snd_seq_client_info_get_client(c);
        const auto clientName = trimmed(snd_seq_client_info_get_name(c));
        if (number == SND_SEQ_CLIENT_SYSTEM || number == d->client || clientName == "Midi Through") {
            continue;
        }
        snd_seq_port_info_set_client(p, number);
        snd_seq_port_info_set_port(p, -1);
        while (snd_seq_query_next_port(d->seq, p) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(p);
            constexpr unsigned sends = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((caps & sends) != sends || (caps & SND_SEQ_PORT_CAP_NO_EXPORT) != 0) {
                continue;
            }
            const auto portName = trimmed(snd_seq_port_info_get_name(p));
            out.push_back({.id = portId(clientName, portName), .name = portName});
        }
    }
    return out;
}

void AlsaMidi::setTarget(const std::string &id)
{
    if (id == m_target) {
        return;
    }
    m_target = id;
    reconnect();
}

void AlsaMidi::reconnect()
{
    if (d->seq == nullptr) {
        return;
    }
    const auto want = m_target.empty() ? std::pair{-1, -1} : d->find(m_target);
    if (m_connected && want == std::pair{d->targetClient, d->targetPort}) {
        return;
    }
    disconnectNow();
    if (want.first < 0) {
        return;
    }
    // Both ways: the controller's buttons and faders to us, our lights,
    // motors and display to it (when it takes MIDI in; not all do).
    const int in = snd_seq_connect_from(d->seq, d->port, want.first, want.second);
    if (in < 0 && in != -EBUSY) {
        std::println(stderr, "patchprimed: can't connect to {} ({})", m_target, snd_strerror(in));
        return;
    }
    snd_seq_connect_to(d->seq, d->port, want.first, want.second);
    d->targetClient = want.first;
    d->targetPort = want.second;
    setConnected(true);
}

void AlsaMidi::disconnectNow()
{
    if (d->targetClient >= 0) {
        snd_seq_disconnect_from(d->seq, d->port, d->targetClient, d->targetPort);
        snd_seq_disconnect_to(d->seq, d->port, d->targetClient, d->targetPort);
    }
    d->targetClient = -1;
    d->targetPort = -1;
    snd_midi_event_reset_decode(d->decoder);
    setConnected(false);
}

void AlsaMidi::setConnected(bool connected)
{
    if (connected != m_connected) {
        m_connected = connected;
        Q_EMIT connectedChanged();
    }
}

void AlsaMidi::readEvents()
{
    bool announced = false;
    for (;;) {
        snd_seq_event_t *event = nullptr;
        const int result = snd_seq_event_input(d->seq, &event);
        if (result == -ENOSPC) {
            continue; // our queue overran; events were lost, carry on
        }
        if (result < 0 || event == nullptr) {
            break;
        }
        if (event->source.client == SND_SEQ_CLIENT_SYSTEM) {
            if (isAnnouncement(event->type)) {
                announced = true;
                const auto &addr = event->data.addr;
                if (m_connected &&
                    (event->type == SND_SEQ_EVENT_CLIENT_EXIT || event->type == SND_SEQ_EVENT_PORT_EXIT) &&
                    std::cmp_equal(addr.client, d->targetClient) &&
                    (event->type == SND_SEQ_EVENT_CLIENT_EXIT || std::cmp_equal(addr.port, d->targetPort))) {
                    d->targetClient = -1; // gone: nothing to disconnect
                    setConnected(false);
                }
            }
            continue;
        }
        if (!m_connected || std::cmp_not_equal(event->source.client, d->targetClient) ||
            std::cmp_not_equal(event->source.port, d->targetPort)) {
            continue;
        }
        if (event->type == SND_SEQ_EVENT_SYSEX) {
            Q_EMIT received(QByteArray(static_cast<const char *>(event->data.ext.ptr),
                                       static_cast<qsizetype>(event->data.ext.len)));
            continue;
        }
        std::array<unsigned char, 64> buffer{};
        const long length = snd_midi_event_decode(d->decoder, buffer.data(), buffer.size(), event);
        if (length > 0) {
            Q_EMIT received(QByteArray(reinterpret_cast<const char *>(buffer.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                                       static_cast<qsizetype>(length)));
        }
    }
    if (announced) {
        reconnect();
        Q_EMIT portsChanged();
    }
}

void AlsaMidi::send(std::span<const std::uint8_t> message)
{
    if (!m_connected || message.empty()) {
        return;
    }
    snd_seq_event_t event{};
    if (message.front() == 0xF0) {
        // snd_seq_ev_set_sysex, without its macro's sign conversions.
        event.type = SND_SEQ_EVENT_SYSEX;
        event.flags = SND_SEQ_EVENT_LENGTH_VARIABLE;
        event.data.ext.len = static_cast<unsigned>(message.size());
        event.data.ext.ptr = const_cast<std::uint8_t *>(message.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast): ALSA only reads it
    } else {
        snd_midi_event_reset_encode(d->encoder);
        if (snd_midi_event_encode(d->encoder, message.data(), static_cast<long>(message.size()),
                                  &event) <= 0 ||
            event.type == SND_SEQ_EVENT_NONE) {
            return;
        }
    }
    event.source.port = static_cast<unsigned char>(d->port);
    snd_seq_ev_set_subs(&event);
    snd_seq_ev_set_direct(&event);
    snd_seq_event_output_direct(d->seq, &event);
}

} // namespace pp

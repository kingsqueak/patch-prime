// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pp {

// A lock-free single-producer, single-consumer queue of messages (a port
// index and some bytes), between the realtime thread and a plugin's UI.
// Neither side waits: a message that doesn't fit is dropped.
class EventRing
{
public:
    explicit EventRing(std::size_t capacity = std::size_t{1} << 16) : m_data(capacity) {}

    // Producer: queues one message; false when there's no room.
    bool push(std::uint32_t port, std::span<const std::uint8_t> bytes) noexcept
    {
        const std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
        const std::size_t needed = sizeof(port) + sizeof(size) + bytes.size();
        const std::size_t write = m_write.load(std::memory_order_relaxed);
        const std::size_t read = m_read.load(std::memory_order_acquire);
        if (m_data.size() - (write - read) < needed) {
            return false;
        }
        std::size_t at = write;
        copyIn(at, &port, sizeof(port));
        copyIn(at, &size, sizeof(size));
        copyIn(at, bytes.data(), bytes.size());
        m_write.store(at, std::memory_order_release);
        return true;
    }

    // Consumer: hands each queued message to fn(port, bytes), in order.
    // scratch holds a message while fn sees it; one too big for it is skipped.
    template<typename Fn>
    void drain(std::vector<std::uint8_t> &scratch, Fn &&fn) noexcept
    {
        std::size_t read = m_read.load(std::memory_order_relaxed);
        const std::size_t write = m_write.load(std::memory_order_acquire);
        while (read != write) {
            std::uint32_t port = 0;
            std::uint32_t size = 0;
            copyOut(read, &port, sizeof(port));
            copyOut(read, &size, sizeof(size));
            if (size <= scratch.size()) {
                copyOut(read, scratch.data(), size);
                fn(port, std::span<const std::uint8_t>(scratch.data(), size));
            } else {
                read += size;
            }
        }
        m_read.store(read, std::memory_order_release);
    }

private:
    void copyIn(std::size_t &at, const void *from, std::size_t n) noexcept
    {
        const auto *bytes = static_cast<const std::uint8_t *>(from);
        for (std::size_t i = 0; i < n; ++i) {
            m_data[(at + i) % m_data.size()] = bytes[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
        at += n;
    }
    void copyOut(std::size_t &at, void *to, std::size_t n) noexcept
    {
        auto *bytes = static_cast<std::uint8_t *>(to);
        for (std::size_t i = 0; i < n; ++i) {
            bytes[i] = m_data[(at + i) % m_data.size()]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
        at += n;
    }

    std::vector<std::uint8_t> m_data;
    std::atomic<std::size_t> m_write{0}; // total bytes written
    std::atomic<std::size_t> m_read{0};  // total bytes read
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "midiport.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pp {

using Message = std::vector<std::uint8_t>;

// A controller that's always there: records what's sent, and plays back
// what the test says the controller sent.
class FakePort : public MidiPort
{
public:
    [[nodiscard]] bool isConnected() const override { return m_connected; }
    void send(std::span<const std::uint8_t> message) override { sent.emplace_back(message.begin(), message.end()); }

    void setConnected(bool connected)
    {
        m_connected = connected;
        Q_EMIT connectedChanged();
    }
    void feed(const Message &bytes)
    {
        Q_EMIT received(QByteArray(reinterpret_cast<const char *>(bytes.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                                   static_cast<qsizetype>(bytes.size())));
    }
    [[nodiscard]] bool wasSent(const Message &message) const
    {
        return std::ranges::contains(sent, message);
    }
    // Everything the display was sent, as text at its offsets.
    [[nodiscard]] std::string display() const
    {
        std::string text(112, ' ');
        for (const auto &m : sent) {
            if (m.size() > 8 && m[0] == 0xF0 && m[5] == 0x12) {
                for (std::size_t i = 7; i + 1 < m.size(); ++i) {
                    text.at(m[6] + i - 7) = static_cast<char>(m[i]);
                }
            }
        }
        return text;
    }

    std::vector<Message> sent; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes): for the tests to read

private:
    bool m_connected = false;
};

} // namespace pp

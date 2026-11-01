// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "midiport.h"

namespace pp {

void MidiParser::feed(const QByteArray &bytes,
                      const std::function<void(const std::vector<std::uint8_t> &)> &message)
{
    for (const char c : bytes) {
        const auto b = static_cast<std::uint8_t>(c);
        if (b >= 0xF8) {
            continue; // real-time messages (clock, active sensing)
        }
        const bool inSysex = !m_incoming.empty() && m_incoming.front() == 0xF0;
        if (b == 0xF0 || (b >= 0x80 && b != 0xF7)) {
            m_incoming.assign(1, b); // a new message (an unfinished one is dropped)
            continue;
        }
        if (m_incoming.empty()) {
            continue; // data with no status
        }
        m_incoming.push_back(b);
        if (inSysex) {
            if (b == 0xF7) {
                message(m_incoming);
                m_incoming.clear();
            }
            continue;
        }
        const std::uint8_t status = m_incoming.front();
        if (status >= 0xF0) {
            m_incoming.clear(); // other system messages aren't used
            continue;
        }
        const std::size_t length = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
        if (m_incoming.size() == length) {
            message(m_incoming);
            m_incoming.assign(1, status); // running status
        }
    }
}

} // namespace pp

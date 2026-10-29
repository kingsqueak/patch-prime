// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace pp {

// std::less<> so lookups take a string_view without building a std::string.
using Properties = std::map<std::string, std::string, std::less<>>;

// One PipeWire global object as seen in the registry. For nodes and devices the
// properties are the full set from the object's info event, not the reduced set
// the registry announces.
struct GraphObject
{
    std::uint32_t id = 0;
    std::string type; // e.g. "PipeWire:Interface:Node"
    std::uint32_t version = 0;
    Properties props;
};

struct MetadataEntry
{
    std::uint32_t subject = 0;
    std::string key;
    std::string type; // e.g. "Spa:String:JSON"
    std::string value;
};

// Returns props[key], or an empty string when the key is missing.
inline const std::string &prop(const GraphObject &object, std::string_view key)
{
    static const std::string empty;
    const auto it = object.props.find(key);
    return it != object.props.end() ? it->second : empty;
}

} // namespace pp

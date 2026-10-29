// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "endpoint.h"

#include <charconv>
#include <string_view>

namespace pp {

std::string makeEndpointKey(const std::string &mediaClass, const std::string &nodeName)
{
    return mediaClass + '|' + nodeName;
}

std::vector<std::string> parsePositions(const std::string &value)
{
    std::vector<std::string> out;
    std::string current;
    for (const char c : value) {
        if (c == '[' || c == ']' || c == ',' || c == ' ') {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

std::optional<Endpoint> classifyEndpoint(const GraphObject &node, const GraphObject *device)
{
    const auto &mediaClass = prop(node, "media.class");
    const auto &nodeName = prop(node, "node.name");
    if (nodeName.empty() || nodeName.starts_with("patchprime.")) {
        return std::nullopt;
    }

    Endpoint e;
    // "Audio/Source/Virtual" is a source too (e.g. a loopback's capture side).
    if (mediaClass == "Audio/Sink") {
        e.direction = Direction::Output;
    } else if (mediaClass == "Audio/Source" || mediaClass == "Audio/Source/Virtual") {
        e.direction = Direction::Input;
    } else {
        return std::nullopt;
    }

    e.key = makeEndpointKey(mediaClass, nodeName);
    e.nodeName = nodeName;
    e.mediaClass = mediaClass;
    e.description = prop(node, "node.description");
    if (e.description.empty()) {
        e.description = nodeName;
    }
    e.isVirtual = prop(node, "device.id").empty();

    const auto &channels = prop(node, "audio.channels");
    int count = 0;
    if (std::from_chars(channels.data(), channels.data() + channels.size(), count).ec ==
        std::errc{}) {
        e.channels = count;
    }
    e.positions = parsePositions(prop(node, "audio.position"));
    if (e.channels == 0) {
        e.channels = static_cast<int>(e.positions.size());
    }

    e.signature.profile = prop(node, "device.profile.name");
    if (device != nullptr) {
        e.deviceName = prop(*device, "device.name");
        e.signature.deviceSerial = prop(*device, "device.serial");
        e.signature.vendorId = prop(*device, "device.vendor.id");
        e.signature.productId = prop(*device, "device.product.id");
        e.signature.busPath = prop(*device, "device.bus-path");
    }
    return e;
}

Match matchSignature(const Signature &saved, const Signature &live)
{
    if (saved.vendorId.empty() || saved.productId.empty() || saved.vendorId != live.vendorId ||
        saved.productId != live.productId) {
        return Match::None;
    }
    if (!saved.deviceSerial.empty() && saved.deviceSerial == live.deviceSerial) {
        return Match::SameSerial;
    }
    if (!saved.busPath.empty() && saved.busPath == live.busPath) {
        return Match::SameBusPath;
    }
    // Two serials that differ are two different units of the same model.
    if (!saved.deviceSerial.empty() && !live.deviceSerial.empty()) {
        return Match::None;
    }
    return Match::SameModel;
}

bool playsInto(const Endpoint &endpoint)
{
    return endpoint.mediaClass == "Audio/Sink";
}

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "graphtypes.h"

#include <optional>
#include <string>
#include <vector>

namespace pp {

enum class Direction : std::uint8_t
{
    Input,  // a source: microphone, line in, virtual source
    Output, // a sink: speakers, headphones, HDMI, virtual sink
};

// What we remember about where an endpoint comes from, so it can be recognised
// again when its node.name changes: a USB device moved to another port (the
// name of a device without a serial includes the port path), or a card switched
// to another profile (the name ends in the profile, e.g. ".analog-stereo").
struct Signature
{
    std::string deviceSerial; // device.serial (USB serial or vendor_model)
    std::string vendorId;     // device.vendor.id, e.g. "0x0763"
    std::string productId;    // device.product.id
    std::string busPath;      // device.bus-path; changes when moved to another port
    std::string profile;      // device.profile.name

    bool operator==(const Signature &) const = default;
};

// An audio input or output the user can enable, label and (later) route.
struct Endpoint
{
    std::string key; // stable identity: media.class + '|' + node.name
    std::string nodeName;
    std::string mediaClass;
    Direction direction = Direction::Output;
    bool isVirtual = false;             // no backing card (null sinks, loopbacks, ...)
    std::string description;            // node.description, what Plasma shows
    std::string deviceName;             // device.name of the card, empty when virtual
    int channels = 0;                   // audio.channels, 0 when unknown
    std::vector<std::string> positions; // audio.position, e.g. {"FL", "FR"}
    Signature signature;
};

std::string makeEndpointKey(const std::string &mediaClass, const std::string &nodeName);

// Turns a node into an Endpoint when it's an audio input or output the mixer
// should offer. Streams, MIDI, video, driver nodes, Patch Prime's own nodes and
// duplex nodes (not handled yet) return nullopt. `device` is the node's card,
// when known; it supplies the signature.
std::optional<Endpoint> classifyEndpoint(const GraphObject &node, const GraphObject *device);

// "[ FL, FR ]" -> {"FL", "FR"}
std::vector<std::string> parsePositions(const std::string &value);

// How confidently a saved signature identifies the same hardware as a live one.
// None for virtual endpoints or unrelated hardware.
enum class Match : std::uint8_t
{
    None,
    SameModel,   // same vendor and product, no other distinguishing info
    SameBusPath, // same model on the same port: a profile change
    SameSerial,  // same model and serial: the same unit, on any port
};

Match matchSignature(const Signature &saved, const Signature &live);

} // namespace pp

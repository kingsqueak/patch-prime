// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "graphtypes.h"

#include <chrono>
#include <expected>
#include <string>
#include <vector>

namespace pp {

struct ServerInfo
{
    std::string name;
    std::string version;        // version of the PipeWire server
    std::string libraryVersion; // version of libpipewire this program runs against
};

// A one-shot copy of the PipeWire graph: nodes, devices, ports, links and the
// "default" metadata. Phase 1 replaces this with a live registry model; the
// probe and early tests use the snapshot.
struct GraphSnapshot
{
    ServerInfo server;
    std::vector<GraphObject> nodes;
    std::vector<GraphObject> devices;
    std::vector<GraphObject> ports;
    std::vector<GraphObject> links;
    std::vector<MetadataEntry> defaults; // entries of the metadata named "default"
};

struct SnapshotOptions
{
    std::chrono::milliseconds timeout{5000};
};

// Connects to the PipeWire server, collects a snapshot, and disconnects.
// Returns an error message when the server can't be reached or doesn't answer
// within the timeout.
std::expected<GraphSnapshot, std::string> takeGraphSnapshot(const SnapshotOptions &options = {});

// The "default" metadata stores device choices as JSON such as
// {"name":"alsa_output.pci-0000_0b_00.6.analog-stereo"}. Returns the node name,
// or an empty string when the value isn't in that form.
std::string defaultNodeName(const std::string &jsonValue);

} // namespace pp

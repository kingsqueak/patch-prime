// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// patchprime-probe: connects to PipeWire once and prints the nodes, devices,
// ports, links and default devices, as JSON or as a short summary.

#include "graphsnapshot.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <print>
#include <string_view>

namespace {

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

QJsonObject toJson(const pp::Properties &props)
{
    QJsonObject out;
    for (const auto &[key, value] : props) {
        out.insert(qs(key), qs(value));
    }
    return out;
}

QJsonArray toJson(const std::vector<pp::GraphObject> &objects)
{
    QJsonArray out;
    for (const auto &object : objects) {
        out.append(QJsonObject{
            {QStringLiteral("id"), static_cast<qint64>(object.id)},
            {QStringLiteral("type"), qs(object.type)},
            {QStringLiteral("version"), static_cast<qint64>(object.version)},
            {QStringLiteral("props"), toJson(object.props)},
        });
    }
    return out;
}

QJsonArray defaultsToJson(const std::vector<pp::MetadataEntry> &entries)
{
    QJsonArray out;
    for (const auto &entry : entries) {
        QJsonValue value = qs(entry.value);
        if (entry.type == "Spa:String:JSON") {
            const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(entry.value));
            if (parsed.isObject()) {
                value = parsed.object();
            } else if (parsed.isArray()) {
                value = parsed.array();
            }
        }
        out.append(QJsonObject{
            {QStringLiteral("subject"), static_cast<qint64>(entry.subject)},
            {QStringLiteral("key"), qs(entry.key)},
            {QStringLiteral("type"), qs(entry.type)},
            {QStringLiteral("value"), value},
        });
    }
    return out;
}

QJsonObject toJson(const pp::GraphSnapshot &snapshot)
{
    return QJsonObject{
        {QStringLiteral("server"),
         QJsonObject{
             {QStringLiteral("name"), qs(snapshot.server.name)},
             {QStringLiteral("version"), qs(snapshot.server.version)},
             {QStringLiteral("libraryVersion"), qs(snapshot.server.libraryVersion)},
         }},
        {QStringLiteral("nodes"), toJson(snapshot.nodes)},
        {QStringLiteral("devices"), toJson(snapshot.devices)},
        {QStringLiteral("ports"), toJson(snapshot.ports)},
        {QStringLiteral("links"), toJson(snapshot.links)},
        {QStringLiteral("defaults"), defaultsToJson(snapshot.defaults)},
    };
}

std::string prop(const pp::GraphObject &object, const std::string &key)
{
    const auto it = object.props.find(key);
    return it != object.props.end() ? it->second : std::string{};
}

std::string defaultFor(const pp::GraphSnapshot &snapshot, std::string_view key)
{
    const auto it = std::ranges::find_if(snapshot.defaults, [key](const pp::MetadataEntry &e) {
        return e.subject == 0 && e.key == key;
    });
    return it != snapshot.defaults.end() ? pp::defaultNodeName(it->value) : std::string{};
}

int portCount(const pp::GraphSnapshot &snapshot, std::uint32_t nodeId, std::string_view direction)
{
    const auto id = std::to_string(nodeId);
    return static_cast<int>(std::ranges::count_if(snapshot.ports, [&](const pp::GraphObject &p) {
        return prop(p, "node.id") == id && prop(p, "port.direction") == direction &&
               prop(p, "port.monitor") != "true";
    }));
}

void printSummary(const pp::GraphSnapshot &snapshot)
{
    const auto defaultSink = defaultFor(snapshot, "default.audio.sink");
    const auto defaultSource = defaultFor(snapshot, "default.audio.source");

    std::println("PipeWire {} (library {})\n", snapshot.server.version,
                 snapshot.server.libraryVersion);

    // Cards come first: a card whose profile is "off" (nothing plugged into its
    // jacks, no monitor on HDMI) has no nodes and would otherwise not appear.
    std::println("Cards");
    for (const auto &device : snapshot.devices) {
        if (prop(device, "media.class") != "Audio/Device") {
            continue;
        }
        const auto id = std::to_string(device.id);
        const auto nodes = std::ranges::count_if(
            snapshot.nodes, [&](const pp::GraphObject &n) { return prop(n, "device.id") == id; });
        const auto status = nodes > 0 ? std::to_string(nodes) + " nodes"
                                      : std::string("no nodes: profile off or nothing connected");
        std::println("    {:4}  {}\n      {}  ({})", device.id, prop(device, "device.description"),
                     prop(device, "device.name"), status);
    }
    std::println();

    struct Section
    {
        std::string_view title;
        std::string_view mediaClassPrefix;
        std::string_view portDirection;
        const std::string *defaultName = nullptr;
    };
    const std::array sections{
        Section{.title = "Outputs (Audio/Sink)",
                .mediaClassPrefix = "Audio/Sink",
                .portDirection = "in",
                .defaultName = &defaultSink},
        Section{.title = "Inputs (Audio/Source)",
                .mediaClassPrefix = "Audio/Source",
                .portDirection = "out",
                .defaultName = &defaultSource},
        Section{.title = "Duplex (Audio/Duplex)",
                .mediaClassPrefix = "Audio/Duplex",
                .portDirection = "in",
                .defaultName = nullptr},
    };

    for (const auto &section : sections) {
        std::println("{}", section.title);
        bool any = false;
        for (const auto &node : snapshot.nodes) {
            const auto mediaClass = prop(node, "media.class");
            if (!mediaClass.starts_with(section.mediaClassPrefix)) {
                continue;
            }
            any = true;
            const auto name = prop(node, "node.name");
            const bool isDefault = section.defaultName != nullptr && name == *section.defaultName;
            const bool isVirtual = prop(node, "device.id").empty();
            std::println("  {} {:4}  {:<2} ch  {:<8} {}\n      {}", isDefault ? '*' : ' ', node.id,
                         portCount(snapshot, node.id, section.portDirection),
                         isVirtual ? "virtual" : "hardware", prop(node, "node.description"), name);
        }
        if (!any) {
            std::println("  (none)");
        }
        std::println();
    }
    std::println("{} nodes, {} devices, {} ports, {} links   (* = system default)",
                 snapshot.nodes.size(), snapshot.devices.size(), snapshot.ports.size(),
                 snapshot.links.size());
}

int run(int &argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("patchprime-probe"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PATCHPRIME_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Print the PipeWire graph (nodes, devices, ports, links, defaults)."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption summaryOption(
        {QStringLiteral("s"), QStringLiteral("summary")},
        QStringLiteral("Print a short device list instead of JSON."));
    const QCommandLineOption compactOption({QStringLiteral("c"), QStringLiteral("compact")},
                                           QStringLiteral("Print JSON on one line."));
    const QCommandLineOption timeoutOption(
        {QStringLiteral("t"), QStringLiteral("timeout")},
        QStringLiteral("Give up after <ms> milliseconds (default 5000)."), QStringLiteral("ms"),
        QStringLiteral("5000"));
    parser.addOptions({summaryOption, compactOption, timeoutOption});
    parser.process(app);

    bool timeoutOk = false;
    const int timeoutMs = parser.value(timeoutOption).toInt(&timeoutOk);
    if (!timeoutOk || timeoutMs <= 0) {
        std::println(stderr, "patchprime-probe: --timeout needs a positive number");
        return 2;
    }

    const auto snapshot = pp::takeGraphSnapshot({.timeout = std::chrono::milliseconds(timeoutMs)});
    if (!snapshot) {
        std::println(stderr, "patchprime-probe: {}", snapshot.error());
        return 1;
    }

    if (parser.isSet(summaryOption)) {
        printSummary(*snapshot);
        return 0;
    }

    const auto format =
        parser.isSet(compactOption) ? QJsonDocument::Compact : QJsonDocument::Indented;
    const auto bytes = QJsonDocument(toJson(*snapshot)).toJson(format);
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::fputs("patchprime-probe: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}

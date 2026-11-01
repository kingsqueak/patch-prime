// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "effects.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QString>

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

// Plugins in a strip's chain: an effect whose type is "lv2:<plugin URI>"
// (parameters keyed by control port symbol), "clap:<plugin id>" (keyed by
// CLAP param id, in decimal) or "vst3:<class id, hex>" (keyed by VST3
// ParamID, in decimal). Each one runs in its own
// patchprime-pluginhost process (see PluginRunner), so a crashing plugin
// can't take the engine down.

constexpr std::string_view lv2TypePrefix = "lv2:";
constexpr std::string_view clapTypePrefix = "clap:";
constexpr std::string_view vst3TypePrefix = "vst3:";

bool isPluginType(std::string_view type);
std::string pluginType(std::string_view uri); // "lv2:" + uri
// The plugin's URI or id (what follows the format), or "" for a built-in type.
std::string pluginUri(std::string_view type);
// "lv2", "clap" or "vst3", or "" for a built-in type.
std::string pluginFormat(std::string_view type);

// What the scan found out about a plugin.
struct PluginInfo
{
    std::string type; // "lv2:<uri>"
    std::string name;
    std::string author;
    std::string category; // "Compressor", "Reverb", ... ("Plugin" when unknown)
    std::string binary;   // CLAP: the .clap file it's in; VST3: its bundle
    int audioIn = 0;      // audio ports, not counting sidechains
    int audioOut = 0;
    std::vector<ParamInfo> params;
    bool hasUi = false; // has its own window that Patch Prime can show
    // Empty when it can run here; otherwise why not (an instrument, a
    // feature Patch Prime doesn't provide).
    std::string problem;
};

QByteArray pluginsToJson(const std::vector<PluginInfo> &plugins);
// The list for clients: everything but the parameters (a plugin can have
// thousands, and the whole list ran to tens of MB), with "paramCount"; and
// one plugin's parameters, fetched when it's used.
QByteArray pluginSummariesToJson(const std::vector<PluginInfo> &plugins);
QByteArray pluginParamsToJson(const PluginInfo &plugin);
std::vector<PluginInfo> pluginsFromJson(const QByteArray &json);

// The plugins installed, from `patchprime-pluginhost --scan`. Scanning
// loads every CLAP and VST3 plugin, which takes a while with many: it's done
// when asked (rescan, the Scan Plugins button), or when there's no list yet.
// The list is cached (~/.cache/patchprime/plugins.json) with the time of
// the scan; a cache from another version of the list is scanned again.
class PluginCatalog : public QObject
{
    Q_OBJECT

public:
    // cachePath: where the list is cached; empty means the default.
    explicit PluginCatalog(QString cachePath = {}, QObject *parent = nullptr);
    // A scan still running is stopped.
    ~PluginCatalog() override;
    PluginCatalog(const PluginCatalog &) = delete;
    PluginCatalog &operator=(const PluginCatalog &) = delete;
    PluginCatalog(PluginCatalog &&) = delete;
    PluginCatalog &operator=(PluginCatalog &&) = delete;

    // A scan that takes longer than this is stopped.
    static constexpr auto scanTimeout = std::chrono::minutes(10);

    // The cache's format; a different one is scanned again.
    static constexpr int cacheVersion = 2;

    // Loads the cache; scans in the background when there's none (or one
    // from another version).
    void start();
    void rescan();
    [[nodiscard]] bool isScanning() const { return m_scan != nullptr; }
    // While scanning: files read of all there are to read (the LV2 ones count
    // as one), and plugins found so far.
    struct Progress
    {
        int done = 0;
        int total = 0;
        int found = 0;
    };
    [[nodiscard]] Progress progress() const { return m_progress; }
    // When the list was scanned (invalid when never).
    [[nodiscard]] QDateTime lastScan() const { return m_lastScan; }

    [[nodiscard]] const std::vector<PluginInfo> &plugins() const { return m_plugins; }
    [[nodiscard]] const PluginInfo *find(const std::string &type) const;
    // Bumped whenever the list changes.
    [[nodiscard]] int revision() const { return m_revision; }
    // For tests: sets the list directly.
    void setPlugins(std::vector<PluginInfo> plugins);

Q_SIGNALS:
    // The list changed, or a scan started or ended.
    void changed();

private:
    void scanFinished(int exitCode, QProcess::ExitStatus status);
    void scanOutput();

    QString m_cachePath;
    std::vector<PluginInfo> m_plugins;
    std::map<std::string, std::size_t> m_index; // type -> position in m_plugins
    QProcess *m_scan = nullptr;
    QByteArray m_output; // the scan's, so far
    qsizetype m_parsed = 0; // how much of it has been read as lines
    Progress m_progress;
    QDateTime m_lastScan;
    int m_revision = 0;
};

// The parameters an effect type has: a built-in's, or a plugin's from the
// catalog (none when it isn't there).
std::vector<ParamInfo> paramsFor(const std::string &type, const PluginCatalog *catalog);

// A strip's chain cut into parts that each run in one node: runs of
// built-in effects (in the engine) and single plugins (each in its own
// process). Effects of unknown types are left out.
struct ChainSegment
{
    bool plugin = false;
    std::vector<std::size_t> indices; // positions in the strip's effect list
};
std::vector<ChainSegment> splitChain(const std::vector<EffectSettings> &effects);

// The patchprime-pluginhost program: $PATCHPRIME_PLUGINHOST, the installed
// one, or the one in the build tree.
QString pluginHostProgram();

} // namespace pp

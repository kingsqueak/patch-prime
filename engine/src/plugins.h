// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "effects.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pp {

// Plugins in a strip's chain: an effect whose type is "lv2:<plugin URI>"
// (parameters keyed by control port symbol) or "clap:<plugin id>"
// (parameters keyed by CLAP param id, in decimal). Each one runs in its own
// patchprime-pluginhost process (see PluginRunner), so a crashing plugin
// can't take the engine down.

constexpr std::string_view lv2TypePrefix = "lv2:";
constexpr std::string_view clapTypePrefix = "clap:";

bool isPluginType(std::string_view type);
std::string pluginType(std::string_view uri); // "lv2:" + uri
// The plugin's URI or id (what follows the format), or "" for a built-in type.
std::string pluginUri(std::string_view type);
// "lv2" or "clap", or "" for a built-in type.
std::string pluginFormat(std::string_view type);

// What the scan found out about a plugin.
struct PluginInfo
{
    std::string type; // "lv2:<uri>"
    std::string name;
    std::string author;
    std::string category; // "Compressor", "Reverb", ... ("Plugin" when unknown)
    std::string binary;   // CLAP: the .clap file it's in
    int audioIn = 0;      // audio ports, not counting sidechains
    int audioOut = 0;
    std::vector<ParamInfo> params;
    bool hasUi = false; // has its own window that Patch Prime can show
    // Empty when it can run here; otherwise why not (an instrument, a
    // feature Patch Prime doesn't provide).
    std::string problem;
};

QByteArray pluginsToJson(const std::vector<PluginInfo> &plugins);
std::vector<PluginInfo> pluginsFromJson(const QByteArray &json);

// The plugins installed, from `patchprime-pluginhost --scan`. The last scan
// is cached (~/.cache/patchprime/plugins.json), so the list is there at
// once on the next start while a fresh scan runs.
class PluginCatalog : public QObject
{
    Q_OBJECT

public:
    // cachePath: where the list is cached; empty means the default.
    explicit PluginCatalog(QString cachePath = {}, QObject *parent = nullptr);

    // Loads the cache, then scans in the background.
    void start();
    void rescan();
    [[nodiscard]] bool isScanning() const { return m_scan != nullptr; }

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

    QString m_cachePath;
    std::vector<PluginInfo> m_plugins;
    std::map<std::string, std::size_t> m_index; // type -> position in m_plugins
    QProcess *m_scan = nullptr;
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

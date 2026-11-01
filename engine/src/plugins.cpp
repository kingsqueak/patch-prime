// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "plugins.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPlugins, "patchprime.plugins")

namespace pp {

namespace {

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string str(const QJsonObject &o, const char *name)
{
    return o.value(QLatin1String(name)).toString().toStdString();
}

QJsonObject paramToJson(const ParamInfo &p)
{
    QJsonArray options;
    for (const auto &o : p.options) {
        options.append(qs(o));
    }
    return {
        {QStringLiteral("name"), qs(p.name)},
        {QStringLiteral("label"), qs(p.label)},
        {QStringLiteral("unit"), qs(p.unit)},
        {QStringLiteral("minimum"), p.minimum},
        {QStringLiteral("maximum"), p.maximum},
        {QStringLiteral("initial"), p.initial},
        {QStringLiteral("kind"), qs(paramKindName(p.kind))},
        {QStringLiteral("options"), options},
    };
}

ParamInfo paramFromJson(const QJsonObject &o)
{
    ParamInfo p;
    p.name = str(o, "name");
    p.label = str(o, "label");
    p.unit = str(o, "unit");
    p.minimum = o.value(QLatin1String("minimum")).toDouble();
    p.maximum = o.value(QLatin1String("maximum")).toDouble();
    p.initial = o.value(QLatin1String("initial")).toDouble();
    const auto kind = str(o, "kind");
    p.kind = ParamKind::Linear;
    if (kind == "log") {
        p.kind = ParamKind::Log;
    } else if (kind == "choice") {
        p.kind = ParamKind::Choice;
    } else if (kind == "toggle") {
        p.kind = ParamKind::Toggle;
    }
    for (const auto &option : o.value(QLatin1String("options")).toArray()) {
        p.options.push_back(option.toString().toStdString());
    }
    return p;
}

QString defaultCachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/patchprime/plugins.json");
}

} // namespace

bool isPluginType(std::string_view type)
{
    return !pluginFormat(type).empty();
}

std::string pluginFormat(std::string_view type)
{
    if (type.starts_with(lv2TypePrefix) && type.size() > lv2TypePrefix.size()) {
        return "lv2";
    }
    if (type.starts_with(clapTypePrefix) && type.size() > clapTypePrefix.size()) {
        return "clap";
    }
    if (type.starts_with(vst3TypePrefix) && type.size() > vst3TypePrefix.size()) {
        return "vst3";
    }
    return {};
}

std::string pluginType(std::string_view uri)
{
    return std::string(lv2TypePrefix) + std::string(uri);
}

std::string pluginUri(std::string_view type)
{
    const auto format = pluginFormat(type);
    return format.empty() ? std::string() : std::string(type.substr(format.size() + 1));
}

QByteArray pluginsToJson(const std::vector<PluginInfo> &plugins)
{
    QJsonArray list;
    for (const auto &p : plugins) {
        QJsonArray params;
        for (const auto &param : p.params) {
            params.append(paramToJson(param));
        }
        list.append(QJsonObject{
            {QStringLiteral("type"), qs(p.type)},
            {QStringLiteral("name"), qs(p.name)},
            {QStringLiteral("author"), qs(p.author)},
            {QStringLiteral("binary"), qs(p.binary)},
            {QStringLiteral("category"), qs(p.category)},
            {QStringLiteral("audioIn"), p.audioIn},
            {QStringLiteral("audioOut"), p.audioOut},
            {QStringLiteral("params"), params},
            {QStringLiteral("hasUi"), p.hasUi},
            {QStringLiteral("problem"), qs(p.problem)},
        });
    }
    return QJsonDocument(list).toJson(QJsonDocument::Compact);
}

QByteArray pluginSummariesToJson(const std::vector<PluginInfo> &plugins)
{
    QJsonArray list;
    for (const auto &p : plugins) {
        list.append(QJsonObject{
            {QStringLiteral("type"), qs(p.type)},
            {QStringLiteral("name"), qs(p.name)},
            {QStringLiteral("author"), qs(p.author)},
            {QStringLiteral("category"), qs(p.category)},
            {QStringLiteral("audioIn"), p.audioIn},
            {QStringLiteral("audioOut"), p.audioOut},
            {QStringLiteral("paramCount"), static_cast<int>(p.params.size())},
            {QStringLiteral("hasUi"), p.hasUi},
            {QStringLiteral("problem"), qs(p.problem)},
        });
    }
    return QJsonDocument(list).toJson(QJsonDocument::Compact);
}

QByteArray pluginParamsToJson(const PluginInfo &plugin)
{
    QJsonArray params;
    for (const auto &param : plugin.params) {
        params.append(paramToJson(param));
    }
    return QJsonDocument(params).toJson(QJsonDocument::Compact);
}

std::vector<PluginInfo> pluginsFromJson(const QByteArray &json)
{
    std::vector<PluginInfo> plugins;
    for (const auto &v : QJsonDocument::fromJson(json).array()) {
        const auto o = v.toObject();
        PluginInfo p;
        p.type = str(o, "type");
        if (!isPluginType(p.type)) {
            continue;
        }
        p.name = str(o, "name");
        p.author = str(o, "author");
        p.binary = str(o, "binary");
        p.category = str(o, "category");
        p.audioIn = o.value(QLatin1String("audioIn")).toInt();
        p.audioOut = o.value(QLatin1String("audioOut")).toInt();
        for (const auto &param : o.value(QLatin1String("params")).toArray()) {
            p.params.push_back(paramFromJson(param.toObject()));
        }
        p.hasUi = o.value(QLatin1String("hasUi")).toBool();
        p.problem = str(o, "problem");
        plugins.push_back(std::move(p));
    }
    return plugins;
}

PluginCatalog::PluginCatalog(QString cachePath, QObject *parent)
    : QObject(parent), m_cachePath(cachePath.isEmpty() ? defaultCachePath() : std::move(cachePath))
{
}

PluginCatalog::~PluginCatalog()
{
    if (m_scan != nullptr) {
        m_scan->disconnect();
        m_scan->kill();
        m_scan->waitForFinished(1000);
    }
}

void PluginCatalog::start()
{
    // The cache: {"version", "scannedAt", "plugins": [...]}.
    QFile cache(m_cachePath);
    const auto json = cache.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(cache.readAll()).object()
                                                      : QJsonObject();
    if (json.value(QLatin1String("version")).toInt() != cacheVersion) {
        rescan(); // none yet, or from another version
        return;
    }
    m_lastScan = QDateTime::fromString(json.value(QLatin1String("scannedAt")).toString(), Qt::ISODate);
    setPlugins(pluginsFromJson(QJsonDocument(json.value(QLatin1String("plugins")).toArray())
                                   .toJson(QJsonDocument::Compact)));
    Q_EMIT changed(); // the time of the scan, at least
}

void PluginCatalog::rescan()
{
    if (m_scan != nullptr) {
        return;
    }
    const auto program = pluginHostProgram();
    if (program.isEmpty()) {
        qCWarning(lcPlugins, "patchprime-pluginhost not found: no plugins");
        return;
    }
    m_output.clear();
    m_parsed = 0;
    m_progress = {};
    m_scan = new QProcess(this);
    m_scan->setProcessChannelMode(QProcess::SeparateChannels);
    m_scan->setStandardErrorFile(QProcess::nullDevice());
    connect(m_scan, &QProcess::readyReadStandardOutput, this, &PluginCatalog::scanOutput);
    connect(m_scan, &QProcess::finished, this, &PluginCatalog::scanFinished);
    connect(m_scan, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qCWarning(lcPlugins, "couldn't start the plugin scan");
            m_scan->deleteLater();
            m_scan = nullptr;
            Q_EMIT changed();
        }
    });
    m_scan->start(program, {QStringLiteral("--scan")});
    // A scan that hangs (a plugin stuck loading) is given up on, so a later
    // rescan can run.
    QTimer::singleShot(scanTimeout, m_scan, [scan = m_scan] {
        qCWarning(lcPlugins, "the plugin scan took too long; stopping it");
        scan->kill();
    });
    Q_EMIT changed(); // scanning
}

void PluginCatalog::scanOutput()
{
    // Progress lines as it goes ({"progress": {"done", "total", "found"}}),
    // then the list on the last line.
    m_output += m_scan->readAllStandardOutput();
    bool moved = false;
    qsizetype end = 0;
    while ((end = m_output.indexOf('\n', m_parsed)) >= 0) {
        const QByteArray line = m_output.mid(m_parsed, end - m_parsed);
        m_parsed = end + 1;
        if (line.startsWith("{\"progress\"")) {
            const auto p = QJsonDocument::fromJson(line).object().value(QLatin1String("progress")).toObject();
            m_progress = {.done = p.value(QLatin1String("done")).toInt(),
                          .total = p.value(QLatin1String("total")).toInt(),
                          .found = p.value(QLatin1String("found")).toInt()};
            moved = true;
        }
    }
    if (moved) {
        Q_EMIT changed();
    }
}

void PluginCatalog::scanFinished(int exitCode, QProcess::ExitStatus status)
{
    scanOutput();
    m_scan->deleteLater();
    m_scan = nullptr;
    m_progress = {};
    const QByteArray output = std::move(m_output);
    m_output.clear();
    if (status != QProcess::NormalExit || exitCode != 0) {
        qCWarning(lcPlugins, "the plugin scan failed (exit code %d)", exitCode);
        Q_EMIT changed(); // no longer scanning
        return;
    }
    // Only a list: anything else (a scan cut short) keeps the list and the
    // cache as they were.
    const auto lines = output.trimmed().split('\n');
    const auto document = QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.last());
    if (!document.isArray()) {
        qCWarning(lcPlugins, "the plugin scan's output wasn't a list; keeping the last one");
        Q_EMIT changed(); // no longer scanning
        return;
    }
    auto plugins = pluginsFromJson(document.toJson(QJsonDocument::Compact));
    qCInfo(lcPlugins, "found %zu plugins", plugins.size());
    m_lastScan = QDateTime::currentDateTime();
    QDir().mkpath(QFileInfo(m_cachePath).absolutePath());
    if (QSaveFile cache(m_cachePath); cache.open(QIODevice::WriteOnly)) {
        cache.write(QJsonDocument(QJsonObject{
                                      {QStringLiteral("version"), cacheVersion},
                                      {QStringLiteral("scannedAt"), m_lastScan.toString(Qt::ISODate)},
                                      {QStringLiteral("plugins"), QJsonDocument::fromJson(pluginsToJson(plugins)).array()},
                                  })
                        .toJson(QJsonDocument::Compact));
        cache.commit();
    }
    const int before = m_revision;
    setPlugins(std::move(plugins));
    if (m_revision == before) {
        Q_EMIT changed(); // the same list, but no longer scanning
    }
}

void PluginCatalog::setPlugins(std::vector<PluginInfo> plugins)
{
    std::ranges::sort(plugins, [](const PluginInfo &a, const PluginInfo &b) {
        return QString::compare(qs(a.name), qs(b.name), Qt::CaseInsensitive) < 0;
    });
    // Every detail counts (a plugin moved to another file, a parameter's
    // new range): compared as the cache would store them.
    if (pluginsToJson(plugins) == pluginsToJson(m_plugins)) {
        return;
    }
    m_plugins = std::move(plugins);
    m_index.clear();
    for (std::size_t i = 0; i < m_plugins.size(); ++i) {
        m_index[m_plugins[i].type] = i;
    }
    ++m_revision;
    Q_EMIT changed();
}

const PluginInfo *PluginCatalog::find(const std::string &type) const
{
    const auto it = m_index.find(type);
    return it == m_index.end() ? nullptr : &m_plugins[it->second];
}

std::vector<ParamInfo> paramsFor(const std::string &type, const PluginCatalog *catalog)
{
    if (!isPluginType(type)) {
        return effectParams(type);
    }
    const auto *info = catalog != nullptr ? catalog->find(type) : nullptr;
    return info != nullptr ? info->params : std::vector<ParamInfo>{};
}

std::vector<ChainSegment> splitChain(const std::vector<EffectSettings> &effects)
{
    std::vector<ChainSegment> segments;
    for (std::size_t i = 0; i < effects.size(); ++i) {
        const auto &type = effects[i].type;
        if (isPluginType(type)) {
            segments.push_back({.plugin = true, .indices = {i}});
        } else if (makeEffect(type)) {
            if (segments.empty() || segments.back().plugin) {
                segments.push_back({.plugin = false, .indices = {}});
            }
            segments.back().indices.push_back(i);
        }
    }
    return segments;
}

QString pluginHostProgram()
{
    auto fromEnv = qEnvironmentVariable("PATCHPRIME_PLUGINHOST");
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }
    // The build tree first, so a daemon or test built here uses the host built
    // with it; then the installed one.
    const QDir here(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        here.filePath(QStringLiteral("patchprime-pluginhost")),
        here.filePath(QStringLiteral("../pluginhost/patchprime-pluginhost")),
        QStringLiteral(PATCHPRIME_PLUGINHOST_INSTALLED),
    };
    for (const auto &path : candidates) {
        if (QFileInfo(path).isExecutable()) {
            return QFileInfo(path).canonicalFilePath();
        }
    }
    return {};
}

} // namespace pp

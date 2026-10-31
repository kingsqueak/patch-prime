// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "pluginrunner.h"

#include "plugins.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>

Q_LOGGING_CATEGORY(lcRunner, "patchprime.plugins.runner")

namespace pp {

namespace {

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

QJsonObject paramsJson(const std::map<std::string, double> &params)
{
    QJsonObject o;
    for (const auto &[name, value] : params) {
        o.insert(qs(name), value);
    }
    return o;
}

QByteArray line(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

void readLevels(const QJsonObject &o, EffectLevels &into, bool merge)
{
    const auto groups = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(o.value(QLatin1String("groups")).toInt(), 0)),
        EffectLevels::maxGroups);
    into.groups = groups;
    const auto fill = [&](const char *name, auto &values, bool peak) {
        const auto list = o.value(QLatin1String(name)).toArray();
        for (std::size_t g = 0; g < groups && std::cmp_less(g, list.size()); ++g) {
            const auto v = static_cast<float>(list.at(static_cast<qsizetype>(g)).toDouble());
            values.at(g) = merge && peak ? std::max(values.at(g), v) : v;
        }
    };
    fill("inPeak", into.inPeak, true);
    fill("inRms", into.inRms, false);
    fill("outPeak", into.outPeak, true);
    fill("outRms", into.outRms, false);
}

} // namespace

PluginRunner::PluginRunner(QObject *parent) : QObject(parent) {}

PluginRunner::~PluginRunner()
{
    for (auto &[node, host] : m_hosts) {
        stop(*host);
    }
}

void PluginRunner::want(const std::string &node, const PluginSpec &spec)
{
    auto &slot = m_hosts[node];
    if (!slot) {
        slot = std::make_unique<Host>();
        slot->spec = spec;
        start(node, *slot);
        return;
    }
    Host &host = *slot;
    if (host.spec == spec) {
        return;
    }
    // The window's title and the plugin's state don't restart it: the
    // state came from it, and the title only shows the next time.
    const bool restart = host.spec.type != spec.type || host.spec.binary != spec.binary ||
                         host.spec.positions != spec.positions ||
                         host.spec.sampleRate != spec.sampleRate ||
                         host.spec.owner != spec.owner ||
                         host.spec.description != spec.description;
    const bool settingsChanged = host.spec.enabled != spec.enabled || host.spec.params != spec.params;
    host.spec = spec;
    if (!host.error.empty() || restart || host.process == nullptr) {
        // Changed settings give a plugin that failed another go.
        stop(host);
        host.crashes.clear();
        host.error.clear();
        start(node, host);
        Q_EMIT changed();
        return;
    }
    if (!settingsChanged) {
        return;
    }
    send(host, line({{QStringLiteral("set"),
                      QJsonObject{{QStringLiteral("enabled"), spec.enabled},
                                  {QStringLiteral("params"), paramsJson(spec.params)}}}}));
}

void PluginRunner::prune(const std::set<std::string> &keep)
{
    for (auto it = m_hosts.begin(); it != m_hosts.end();) {
        if (keep.contains(it->first)) {
            ++it;
            continue;
        }
        qCInfo(lcRunner, "stopping plugin node %s", it->first.c_str());
        stop(*it->second);
        it = m_hosts.erase(it);
    }
}

std::string PluginRunner::error(const std::string &node) const
{
    const auto it = m_hosts.find(node);
    return it == m_hosts.end() ? std::string() : it->second->error;
}

void PluginRunner::setMetering(const std::string &node, bool on)
{
    const auto it = m_hosts.find(node);
    if (it == m_hosts.end() || it->second->metering == on) {
        return;
    }
    it->second->metering = on;
    send(*it->second, line({{QStringLiteral("metering"), on}}));
}

void PluginRunner::setWindow(const std::string &node, bool open)
{
    const auto it = m_hosts.find(node);
    if (it == m_hosts.end()) {
        return;
    }
    it->second->windowError.clear();
    send(*it->second, line({{QStringLiteral("window"), open}}));
}

bool PluginRunner::isWindowOpen(const std::string &node) const
{
    const auto it = m_hosts.find(node);
    return it != m_hosts.end() && it->second->windowOpen;
}

std::string PluginRunner::windowError(const std::string &node) const
{
    const auto it = m_hosts.find(node);
    return it == m_hosts.end() ? std::string() : it->second->windowError;
}

void PluginRunner::requestState(const std::string &node)
{
    if (const auto it = m_hosts.find(node); it != m_hosts.end()) {
        send(*it->second, line({{QStringLiteral("saveState"), true}}));
    }
}

std::optional<EffectLevels> PluginRunner::takeLevels(const std::string &node)
{
    const auto it = m_hosts.find(node);
    if (it == m_hosts.end() || !it->second->hasLevels) {
        return std::nullopt;
    }
    it->second->hasLevels = false;
    return it->second->levels;
}

void PluginRunner::start(const std::string &node, Host &host)
{
    const auto program = pluginHostProgram();
    if (program.isEmpty()) {
        host.error = "patchprime-pluginhost isn't installed";
        return;
    }
    QStringList positions;
    for (const auto &p : host.spec.positions) {
        positions.append(qs(p));
    }
    QStringList args{
        QStringLiteral("--run"),
        QStringLiteral("--format"), qs(pluginFormat(host.spec.type)),
        QStringLiteral("--uri"), qs(pluginUri(host.spec.type)),
        QStringLiteral("--binary"), qs(host.spec.binary),
        QStringLiteral("--name"), qs(node),
        QStringLiteral("--positions"), positions.join(QLatin1Char(',')),
        QStringLiteral("--rate"), QString::number(host.spec.sampleRate),
        QStringLiteral("--owner"), qs(host.spec.owner),
        QStringLiteral("--description"), qs(host.spec.description),
        QStringLiteral("--title"), qs(host.spec.title),
        QStringLiteral("--params"),
        QString::fromUtf8(QJsonDocument(paramsJson(host.spec.params)).toJson(QJsonDocument::Compact)),
    };
    if (!host.spec.enabled) {
        args.append(QStringLiteral("--bypassed"));
    }
    if (!host.spec.state.empty()) {
        args.append(QStringLiteral("--state-follows"));
    }
    host.pending.clear();
    host.reported.clear();
    host.hasLevels = false;
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, node] { readOutput(node); });
    connect(process, &QProcess::finished, this,
            [this, node](int exitCode, QProcess::ExitStatus status) {
                finished(node, exitCode, status);
            });
    connect(process, &QProcess::errorOccurred, this, [this, node](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            finished(node, 2, QProcess::NormalExit);
        }
    });
    host.process = process;
    qCInfo(lcRunner, "starting plugin node %s (%s)", node.c_str(), host.spec.type.c_str());
    process->start(program, args);
    if (!host.spec.state.empty()) {
        send(host, line({{QStringLiteral("state"), qs(host.spec.state)}}));
    }
    if (host.metering) {
        send(host, line({{QStringLiteral("metering"), true}}));
    }
}

void PluginRunner::stop(Host &host)
{
    if (host.restart != nullptr) {
        host.restart->stop();
        host.restart->deleteLater();
        host.restart = nullptr;
    }
    if (host.process == nullptr) {
        return;
    }
    QProcess *process = host.process;
    host.process = nullptr;
    process->disconnect();
    process->kill();
    process->waitForFinished(1000);
    process->deleteLater();
}

void PluginRunner::finished(const std::string &node, int exitCode, QProcess::ExitStatus status)
{
    const auto it = m_hosts.find(node);
    if (it == m_hosts.end() || it->second->process == nullptr) {
        return;
    }
    Host &host = *it->second;
    readOutput(node);
    host.windowOpen = false;
    host.process->disconnect();
    host.process->deleteLater();
    host.process = nullptr;

    if (status == QProcess::NormalExit && exitCode == 2) {
        // The plugin couldn't be loaded: no point trying again as it is.
        host.error = host.reported.empty() ? "the plugin couldn't be loaded" : host.reported;
        qCWarning(lcRunner, "plugin node %s: %s", node.c_str(), host.error.c_str());
        Q_EMIT changed();
        return;
    }
    const auto now = Clock::now();
    std::erase_if(host.crashes, [now](Clock::time_point t) { return now - t > crashWindow; });
    host.crashes.push_back(now);
    if (std::cmp_greater_equal(host.crashes.size(), crashLimit)) {
        host.error = "it crashed " + std::to_string(host.crashes.size()) +
                     " times, so it's off; change a setting to try again";
        qCWarning(lcRunner, "plugin node %s: %s", node.c_str(), host.error.c_str());
        Q_EMIT changed();
        return;
    }
    qCWarning(lcRunner, "plugin node %s stopped (exit %d); restarting", node.c_str(), exitCode);
    host.restart = new QTimer(this);
    host.restart->setSingleShot(true);
    connect(host.restart, &QTimer::timeout, this, [this, node] {
        const auto h = m_hosts.find(node);
        if (h == m_hosts.end() || h->second->process != nullptr) {
            return;
        }
        h->second->restart->deleteLater();
        h->second->restart = nullptr;
        start(node, *h->second);
        Q_EMIT changed();
    });
    host.restart->start(restartDelay);
    Q_EMIT changed();
}

void PluginRunner::readOutput(const std::string &node)
{
    const auto it = m_hosts.find(node);
    if (it == m_hosts.end() || it->second->process == nullptr) {
        return;
    }
    Host &host = *it->second;
    host.pending += host.process->readAllStandardOutput();
    qsizetype end = 0;
    while ((end = host.pending.indexOf('\n')) >= 0) {
        const auto o = QJsonDocument::fromJson(host.pending.left(end)).object();
        host.pending.remove(0, end + 1);
        if (const auto levels = o.value(QLatin1String("levels")); levels.isObject()) {
            readLevels(levels.toObject(), host.levels, host.hasLevels);
            host.hasLevels = true;
        } else if (const auto error = o.value(QLatin1String("error")); error.isString()) {
            host.reported = error.toString().toStdString();
        } else if (const auto window = o.value(QLatin1String("window")); window.isBool()) {
            host.windowOpen = window.toBool();
            Q_EMIT changed();
        } else if (const auto failed = o.value(QLatin1String("windowError")); failed.isString()) {
            host.windowError = failed.toString().toStdString();
            Q_EMIT changed();
        } else if (const auto param = o.value(QLatin1String("param")); param.isObject()) {
            const auto p = param.toObject();
            const auto name = p.value(QLatin1String("name")).toString().toStdString();
            const double value = p.value(QLatin1String("value")).toDouble();
            host.spec.params[name] = value; // it has it already: no need to send it back
            Q_EMIT paramChanged(node, name, value);
        } else if (const auto state = o.value(QLatin1String("state")); state.isString()) {
            host.spec.state = state.toString().toStdString();
            Q_EMIT stateChanged(node, host.spec.state);
        }
    }
}

void PluginRunner::send(Host &host, const QByteArray &message)
{
    if (host.process != nullptr && host.process->state() != QProcess::NotRunning) {
        host.process->write(message);
    }
}

} // namespace pp

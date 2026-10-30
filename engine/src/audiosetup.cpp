// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "audiosetup.h"

#include "devicemodel.h"
#include "pipewiregraph.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

Q_LOGGING_CATEGORY(lcSetup, "patchprime.setup")

namespace pp {

namespace {

// Settings values compare without spaces: PipeWire may echo "[ 48000 ]" for
// "[48000]".
std::string compact(std::string value)
{
    std::erase(value, ' ');
    return value;
}

} // namespace

int largestBlock(const DeviceStore &store)
{
    // Automatic: PipeWire may go up to its clock.max-quantum, 2048 by default.
    return store.blockSize() > 0 ? store.blockSize() : 2048;
}

int followerPeriods(const DeviceStore &store)
{
    // Two blocks at the smallest period size seen (512 frames); devices with
    // bigger periods just get a bigger, equally safe buffer.
    constexpr int smallestPeriod = 512;
    return std::max(4, (2 * largestBlock(store) + smallestPeriod - 1) / smallestPeriod);
}

std::map<std::string, std::string> wantedSettings(const DeviceStore &store)
{
    const auto rate = std::to_string(store.sampleRate());
    return {
        {"clock.rate", rate},
        {"clock.allowed-rates", "[ " + rate + " ]"},
        {"clock.force-quantum", std::to_string(store.blockSize())},
    };
}

int clockDevicePriority()
{
    return 30000; // above any hardware (about 1000-2600) and Patch Prime's buses
}

std::string wirePlumberRules(const DeviceStore &store)
{
    // The chosen master keeps interrupt scheduling (it is the clock); every
    // other node follows on a timer, its card's other nodes included: they
    // follow too whenever the master's node is gone but the card stays (an
    // HDMI port unplugged, say), and a timer follows the same clock fine.
    const auto *clockDevice = store.clock() == ClockMode::Hardware
                                  ? store.find(store.clockDevice())
                                  : nullptr;

    std::string rules;
    for (const auto &[key, s] : store.entries()) {
        if (s.kind != EndpointKind::Hardware || !s.nodeName.starts_with("alsa_")) {
            continue;
        }
        const bool isMaster = clockDevice != nullptr && key == store.clockDevice();
        std::vector<std::string> props;
        // Automatic: a timer for anything that follows another clock: every
        // node but the chosen master (Patch Prime's buses clock the graph
        // otherwise, and while the master is unplugged).
        const bool follows = !isMaster;
        const bool timer = s.scheduling == Scheduling::Timer ||
                           (s.scheduling == Scheduling::Auto && follows);
        if (timer || s.scheduling == Scheduling::Interrupt) {
            props.push_back(std::string("api.alsa.disable-tsched = ") + (timer ? "false" : "true"));
        }
        if (s.sampleRate > 0) {
            props.push_back("audio.rate = " + std::to_string(s.sampleRate));
        }
        if (clockDevice != nullptr && key == store.clockDevice()) {
            props.push_back("priority.driver = " + std::to_string(clockDevicePriority()));
        }
        // A follower on a timer gets a small hardware buffer. When another
        // interface vanishes, the graph stalls for a moment and the device
        // replays whatever is left in its buffer: with PipeWire's default
        // (as many periods as possible, ~0.7 s) that was a loud noise burst;
        // with a few periods it's a few milliseconds.
        if (timer && !isMaster) {
            props.push_back("api.alsa.period-num = " + std::to_string(followerPeriods(store)));
        }
        if (props.empty()) {
            continue;
        }
        rules += "  {\n    matches = [ { node.name = \"" + s.nodeName +
                 "\" } ]\n    actions = { update-props = { ";
        for (std::size_t i = 0; i < props.size(); ++i) {
            rules += (i > 0 ? ", " : "") + props[i];
        }
        rules += " } }\n  }\n";
    }
    return "# Written by Patch Prime from its device settings; changes here are overwritten.\n"
           "monitor.alsa.rules = [\n" +
           rules + "]\n";
}

AudioSetup::AudioSetup(PipeWireGraph *graph, DeviceModel *devices, QString rulesPath,
                       bool restartWirePlumber, QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices),
      m_rulesPath(rulesPath.isEmpty() ? defaultRulesPath() : std::move(rulesPath)),
      m_restartWirePlumber(restartWirePlumber)
{
    m_applyTimer.setSingleShot(true);
    m_applyTimer.setInterval(50);
    connect(&m_applyTimer, &QTimer::timeout, this, &AudioSetup::applySettings);

    connect(graph, &PipeWireGraph::settingsChanged, this, &AudioSetup::scheduleApply);
    connect(graph, &PipeWireGraph::connectedChanged, this, &AudioSetup::scheduleApply);
    connect(devices, &DeviceModel::audioSettingsChanged, this, [this] {
        scheduleApply();
        writeRules();
    });
    // Devices seen for the first time (or under a new node.name) need rules too.
    connect(devices, &DeviceModel::settingsChanged, this, &AudioSetup::writeRules);
    writeRules();
}

QString AudioSetup::defaultRulesPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/wireplumber/wireplumber.conf.d/60-patchprime.conf");
}

bool AudioSetup::writeRules()
{
    const auto text = QByteArray::fromStdString(wirePlumberRules(m_devices->store()));
    QFile current(m_rulesPath);
    if (current.open(QIODevice::ReadOnly) && current.readAll() == text) {
        return false;
    }
    current.close();
    QDir().mkpath(QFileInfo(m_rulesPath).absolutePath());
    QSaveFile file(m_rulesPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
        qCWarning(lcSetup, "could not write %s: %s", qPrintable(m_rulesPath),
                  qPrintable(file.errorString()));
        return false;
    }
    qCInfo(lcSetup, "wrote %s", qPrintable(m_rulesPath));
    if (m_restartWirePlumber) {
        // WirePlumber reads its rules at startup. Devices come back within a
        // few seconds, and the buses link to them again.
        qCInfo(lcSetup, "restarting WirePlumber to apply the device settings");
        QProcess::startDetached(
            QStringLiteral("systemctl"),
            {QStringLiteral("--user"), QStringLiteral("restart"), QStringLiteral("wireplumber")});
    }
    return true;
}

void AudioSetup::scheduleApply()
{
    if (!m_applyTimer.isActive()) {
        m_applyTimer.start();
    }
}

void AudioSetup::applySettings()
{
    if (!m_graph->isConnected()) {
        return;
    }
    for (const auto &[key, value] : wantedSettings(m_devices->store())) {
        const auto current = m_graph->setting(key);
        // Unknown yet: the settings metadata hasn't been seen; it will be.
        if (current.empty() || compact(current) == compact(value)) {
            continue;
        }
        qCInfo(lcSetup, "setting %s to %s (was %s)", key.c_str(), value.c_str(), current.c_str());
        m_graph->setSetting(key, value);
    }
}

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "hotplugrecovery.h"

#include "devicemodel.h"
#include "devicestore.h"
#include "pipewiregraph.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>

#include <algorithm>
#include <set>

Q_LOGGING_CATEGORY(lcHotplug, "patchprime.hotplug")

namespace pp {

std::vector<HalfAttached> findHalfAttached(const std::map<std::uint32_t, GraphObject> &nodes,
                                           const std::map<std::uint32_t, GraphObject> &devices,
                                           const DeviceStore &store)
{
    std::set<std::string> nodeNames;
    std::map<std::string, std::set<std::string>> cardProfiles; // card -> profiles its nodes run in
    for (const auto &[id, node] : nodes) {
        const auto &name = prop(node, "node.name");
        nodeNames.insert(name);
        if (const auto card = alsaCardOf(name); !card.empty()) {
            cardProfiles["alsa_card." + card].insert(prop(node, "device.profile.name"));
        }
    }
    std::set<std::string> cards;
    for (const auto &[id, device] : devices) {
        cards.insert(prop(device, "device.name"));
    }

    std::vector<HalfAttached> out;
    for (const auto &[key, s] : store.entries()) {
        const auto card = alsaCardOf(s.nodeName);
        if (!s.enabled || s.kind != EndpointKind::Hardware || card.empty() ||
            nodeNames.contains(s.nodeName)) {
            continue;
        }
        const auto deviceName = "alsa_card." + card;
        if (!cards.contains(deviceName)) {
            continue; // unplugged
        }
        const auto running = cardProfiles.find(deviceName);
        if (running == cardProfiles.end()) {
            // No nodes at all: taken entirely by another session, or switched
            // off; the card's active profile tells which.
            out.push_back({.card = deviceName, .key = key, .noNodes = true});
        } else if (!s.signature.profile.empty() && running->second.contains(s.signature.profile)) {
            // A sibling runs in the device's profile, so its node should exist.
            // A card switched to another profile on purpose doesn't count.
            out.push_back({.card = deviceName, .key = key, .noNodes = false});
        }
    }
    return out;
}

std::string activeCardProfile(const QByteArray &pactlJson, const std::string &card)
{
    const auto cardName = QString::fromStdString(card);
    for (const auto &value : QJsonDocument::fromJson(pactlJson).array()) {
        const auto object = value.toObject();
        if (object.value(QLatin1String("name")).toString() == cardName) {
            return object.value(QLatin1String("active_profile")).toString().toStdString();
        }
    }
    return {};
}

HotplugRecovery::HotplugRecovery(PipeWireGraph *graph, DeviceModel *devices, QObject *parent)
    : QObject(parent), m_graph(graph), m_devices(devices)
{
    // Checked once a second: cheap, and a half-attached card shows no event
    // of its own (its missing node simply never comes).
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &HotplugRecovery::check);
    m_timer.start();
}

void HotplugRecovery::check()
{
    if (!m_graph->isConnected()) {
        m_seen.clear();
        return;
    }
    const auto now = Clock::now();
    const auto found = findHalfAttached(m_graph->nodes(), m_graph->devices(), m_devices->store());

    std::set<std::string> current;
    for (const auto &h : found) {
        current.insert(h.card);
    }
    std::erase_if(m_seen, [&current](const auto &e) { return !current.contains(e.first); });
    std::erase_if(m_leaveAlone, [now](const auto &e) { return now >= e.second; });

    std::set<std::string> handled;
    for (const auto &h : found) {
        if (handled.contains(h.card) || m_leaveAlone.contains(h.card)) {
            continue; // one reset per card covers all its nodes
        }
        handled.insert(h.card);
        const auto [seen, isNew] = m_seen.try_emplace(h.card, now);
        if (now - seen->second < settleTime) {
            continue;
        }
        m_seen.erase(h.card); // wait settleTime again before anything else
        auto &attempts = m_attempts[h.card];
        std::erase_if(attempts, [now](auto t) { return now - t > window; });
        if (std::cmp_greater_equal(attempts.size(), maxAttempts)) {
            continue;
        }

        // What the card is set to: re-apply exactly that, or leave a card
        // that's switched off alone.
        QProcess pactl;
        pactl.start(QStringLiteral("pactl"), {QStringLiteral("--format=json"),
                                              QStringLiteral("list"), QStringLiteral("cards")});
        const bool listed = pactl.waitForFinished(3000) && pactl.exitCode() == 0;
        const auto profile =
            listed ? activeCardProfile(pactl.readAllStandardOutput(), h.card) : std::string{};
        if (profile.empty()) {
            continue; // couldn't tell (pactl failed or timed out): try again next time
        }
        if (profile == "off") {
            if (const auto mine = m_switchedOff.find(h.card); mine != m_switchedOff.end()) {
                // We switched it off and putting it back failed: try again,
                // never leave it off.
                restore(h.card, mine->second, restoreTries);
            } else if (h.noNodes) {
                m_leaveAlone[h.card] = now + window; // off on purpose: don't ask every few seconds
            }
            continue;
        }

        attempts.push_back(now);
        const int row = m_devices->rowForKey(h.key);
        const auto label =
            row >= 0 ? m_devices->data(m_devices->index(row), DeviceModel::LabelRole).toString()
                     : QString::fromStdString(h.key);
        qCWarning(lcHotplug, "%s came back only partly; re-applying profile %s to %s",
                  qPrintable(label), profile.c_str(), h.card.c_str());
        reset(h.card, profile);
        Q_EMIT recovering(label, QString::fromStdString(h.card));
    }
}

void HotplugRecovery::runPactl(const QStringList &args, const std::function<void(bool)> &done)
{
    auto *process = new QProcess(this);
    // Exactly one answer: finished (with its exit code), or a failure to
    // start at all, which emits no finished.
    connect(process, &QProcess::finished, this,
            [process, done](int code, QProcess::ExitStatus status) {
                process->deleteLater();
                done(status == QProcess::NormalExit && code == 0);
            });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            done(false);
        }
    });
    process->start(QStringLiteral("pactl"), args);
}

void HotplugRecovery::reset(const std::string &card, const std::string &profile)
{
    // The same as switching the card's profile off and back by hand. Once
    // it's off, the card is remembered until it's back on (m_switchedOff), so
    // a failed restore is retried and never taken for "off on purpose".
    const auto cardName = QString::fromStdString(card);
    runPactl({QStringLiteral("set-card-profile"), cardName, QStringLiteral("off")},
             [this, card, profile, cardName](bool ok) {
                 if (!ok) {
                     qCWarning(lcHotplug, "could not switch %s off; leaving it",
                               qPrintable(cardName));
                     return;
                 }
                 m_switchedOff[card] = profile;
                 restore(card, profile, restoreTries);
             });
}

void HotplugRecovery::restore(const std::string &card, const std::string &profile, int triesLeft)
{
    const auto cardName = QString::fromStdString(card);
    runPactl({QStringLiteral("set-card-profile"), cardName, QString::fromStdString(profile)},
             [this, card, profile, cardName, triesLeft](bool ok) {
                 if (ok) {
                     m_switchedOff.erase(card);
                     return;
                 }
                 qCWarning(lcHotplug, "could not set %s back to %s (%d tries left)",
                           qPrintable(cardName), profile.c_str(), triesLeft - 1);
                 if (triesLeft > 1) {
                     QTimer::singleShot(2000, this, [this, card, profile, triesLeft] {
                         restore(card, profile, triesLeft - 1);
                     });
                 }
                 // Out of tries: check() still sees it off and ours, and tries again.
             });
}

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "ctl.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <optional>

namespace pp::ctl {

namespace {

constexpr double defaultStepDb = 1.0;

using Result = std::expected<Plan, QString>;

std::unexpected<QString> fail(const QString &message)
{
    return std::unexpected(message);
}

// The mixer's strips in its order: inputs, mix buses, outputs.
QJsonArray strips(const QJsonObject &state)
{
    QJsonArray out;
    for (const auto *stage : {"input", "bus", "output"}) {
        for (const auto &d : state.value(QLatin1String("devices")).toArray()) {
            const auto strip = d.toObject();
            if (strip.value(QLatin1String("isEnabled")).toBool() &&
                strip.value(QLatin1String("stage")).toString() == QLatin1String(stage)) {
                out.append(d);
            }
        }
    }
    return out;
}

// A strip by label (any case), key or node name; one, or an error.
std::expected<QJsonObject, QString> findStrip(const QJsonObject &state, const QString &name)
{
    std::vector<QJsonObject> found;
    for (const auto &field : {"key", "nodeName", "label"}) {
        for (const auto &d : strips(state)) {
            const auto strip = d.toObject();
            if (strip.value(QLatin1String(field)).toString().compare(name, Qt::CaseInsensitive) == 0) {
                found.push_back(strip);
            }
        }
        if (!found.empty()) {
            break; // a key or node name wins over a label
        }
    }
    if (found.empty()) {
        return std::unexpected(QStringLiteral("no strip called \"%1\" (see: patchprime-ctl list)").arg(name));
    }
    if (found.size() > 1) {
        QStringList keys;
        for (const auto &s : found) {
            keys.append(s.value(QLatin1String("key")).toString());
        }
        return std::unexpected(QStringLiteral("more than one strip is called \"%1\"; use its key: %2")
                                   .arg(name, keys.join(QStringLiteral(", "))));
    }
    return found.front();
}

// on, off or toggle (the default) against the current state.
std::expected<bool, QString> onOff(const QStringList &args, qsizetype at, bool current)
{
    const auto word = args.size() > at ? args.at(at).toLower() : QStringLiteral("toggle");
    if (word == QLatin1String("on") || word == QLatin1String("1") || word == QLatin1String("true")) {
        return true;
    }
    if (word == QLatin1String("off") || word == QLatin1String("0") || word == QLatin1String("false")) {
        return false;
    }
    if (word == QLatin1String("toggle")) {
        return !current;
    }
    return std::unexpected(QStringLiteral("\"%1\": expected on, off or toggle").arg(args.at(at)));
}

// A level: "<dB>", "up [step]" or "down [step]" from current.
std::expected<double, QString> level(const QStringList &args, qsizetype at, double current)
{
    if (args.size() <= at) {
        return std::unexpected(QStringLiteral("missing a level: a number of dB, or up/down [dB]"));
    }
    const auto word = args.at(at).toLower();
    bool ok = false;
    if (word == QLatin1String("up") || word == QLatin1String("down")) {
        double step = defaultStepDb;
        if (args.size() > at + 1) {
            step = args.at(at + 1).toDouble(&ok);
            if (!ok || step < 0) {
                return std::unexpected(QStringLiteral("\"%1\" isn't a step in dB").arg(args.at(at + 1)));
            }
        }
        return current + (word == QLatin1String("up") ? step : -step);
    }
    const double db = QString(word).remove(QStringLiteral("db")).toDouble(&ok);
    if (!ok) {
        return std::unexpected(QStringLiteral("\"%1\" isn't a level in dB").arg(args.at(at)));
    }
    return db;
}

QString key(const QJsonObject &strip)
{
    return strip.value(QLatin1String("key")).toString();
}

bool sendsTo(const QJsonObject &strip, const QString &destination)
{
    return strip.value(QLatin1String("sends")).toArray().contains(destination);
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

QString dbText(double db)
{
    return db <= -60.0 ? QStringLiteral("-inf") : QString::number(db, 'f', 1);
}

Result list(const QStringList &args, const QJsonObject &state)
{
    if (args.contains(QStringLiteral("--json"))) {
        QJsonArray out;
        for (const auto &d : strips(state)) {
            const auto s = d.toObject();
            QJsonObject entry;
            for (const auto *field : {"key", "label", "stage", "volumeDb", "muted", "cued", "sends", "sendLevels"}) {
                entry.insert(QLatin1String(field), s.value(QLatin1String(field)));
            }
            out.append(entry);
        }
        return Plan{.calls = {}, .output = QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented))};
    }
    QString text;
    for (const auto &d : strips(state)) {
        const auto s = d.toObject();
        QStringList flags;
        if (s.value(QLatin1String("muted")).toBool()) {
            flags.append(QStringLiteral("muted"));
        }
        if (s.value(QLatin1String("cued")).toBool()) {
            flags.append(QStringLiteral("cued"));
        }
        text += QStringLiteral("%1  %2  %3 dB%4\n")
                    .arg(s.value(QLatin1String("label")).toString(), -24)
                    .arg(s.value(QLatin1String("stage")).toString(), -6)
                    .arg(dbText(s.value(QLatin1String("volumeDb")).toDouble()), 6)
                    .arg(flags.isEmpty() ? QString() : QStringLiteral("  ") + flags.join(QLatin1Char(' ')));
    }
    return Plan{.calls = {}, .output = text};
}

Result get(const QStringList &args, const QJsonObject &state)
{
    if (args.size() < 2) {
        return fail(QStringLiteral("usage: patchprime-ctl get <strip> [volume|muted|cued]"));
    }
    const auto strip = findStrip(state, args.at(1));
    if (!strip) {
        return fail(strip.error());
    }
    const auto volume = dbText(strip->value(QLatin1String("volumeDb")).toDouble());
    const auto muted = yesNo(strip->value(QLatin1String("muted")).toBool());
    const auto cued = yesNo(strip->value(QLatin1String("cued")).toBool());
    if (args.size() < 3) {
        return Plan{.calls = {},
                    .output = QStringLiteral("volume %1\nmuted %2\ncued %3\n").arg(volume, muted, cued)};
    }
    const auto what = args.at(2).toLower();
    if (what == QLatin1String("volume")) {
        return Plan{.calls = {}, .output = volume + QLatin1Char('\n')};
    }
    if (what == QLatin1String("muted")) {
        return Plan{.calls = {}, .output = muted + QLatin1Char('\n')};
    }
    if (what == QLatin1String("cued")) {
        return Plan{.calls = {}, .output = cued + QLatin1Char('\n')};
    }
    return fail(QStringLiteral("\"%1\": expected volume, muted or cued").arg(args.at(2)));
}

Result scene(const QStringList &args, const QJsonObject &state)
{
    const auto names = state.value(QLatin1String("scenes")).toArray();
    if (args.size() < 2) {
        return fail(QStringLiteral("usage: patchprime-ctl scene <name>"));
    }
    const auto wanted = args.mid(1).join(QLatin1Char(' '));
    for (const auto &n : names) {
        if (n.toString().compare(wanted, Qt::CaseInsensitive) == 0) {
            return Plan{.calls = {{.method = QStringLiteral("LoadScene"), .args = {n.toString()}}}, .output = {}};
        }
    }
    return fail(QStringLiteral("no scene called \"%1\" (see: patchprime-ctl scenes)").arg(wanted));
}

} // namespace

QString usage()
{
    return QStringLiteral(R"(Usage: patchprime-ctl <command> [arguments]

Control Patch Prime's mixer from scripts, hotkey tools and Stream Deck
buttons. A strip is named by its label (any case), or by its key when two
share a label. Switches take on, off or toggle (the default).

  list [--json]                     the mixer's strips, levels, mutes
  get <strip> [volume|muted|cued]   a strip's state (for button states)
  volume <strip> <dB>|up|down [dB]  set or nudge a level (1 dB steps)
  mute <strip> [on|off|toggle]      mute (push to talk: off on press,
                                    on on release)
  cue <strip> [on|off|toggle]       hear it alone on the cue output
  send <from> <to> [on|off|toggle]  send a strip to a mix bus or output
  send-level <from> <to> <dB>|up|down [dB]
  effect <strip> <n> [on|off|toggle]  turn its n-th effect on or off
  record <strip> [on|off|toggle]    record it to the recordings folder
  scene <name>                      load a scene
  scenes                            list the scenes
  pad <name>                        play a soundboard pad (stop it if playing)
  pads [stop]                       list the pads (playing ones marked),
                                    or stop them all

Options: --help, --version
)");
}

std::expected<Plan, QString> plan(const QStringList &args, const QJsonObject &state)
{
    if (args.isEmpty()) {
        return fail(usage());
    }
    const auto command = args.front().toLower();
    if (command == QLatin1String("list")) {
        return list(args, state);
    }
    if (command == QLatin1String("get")) {
        return get(args, state);
    }
    if (command == QLatin1String("scenes")) {
        QString text;
        for (const auto &n : state.value(QLatin1String("scenes")).toArray()) {
            text += n.toString() + QLatin1Char('\n');
        }
        return Plan{.calls = {}, .output = text};
    }
    if (command == QLatin1String("scene")) {
        return scene(args, state);
    }
    if (command == QLatin1String("pads")) {
        if (args.size() > 1 && args.at(1).toLower() == QLatin1String("stop")) {
            return Plan{.calls = {{.method = QStringLiteral("StopAllPads"), .args = {}}}, .output = {}};
        }
        QString text;
        for (const auto &p : state.value(QLatin1String("pads")).toArray()) {
            const auto pad = p.toObject();
            text += pad.value(QLatin1String("label")).toString() +
                    (pad.value(QLatin1String("playing")).toBool() ? QStringLiteral("  (playing)") : QString()) +
                    QLatin1Char('\n');
        }
        return Plan{.calls = {}, .output = text};
    }
    if (command == QLatin1String("pad")) {
        if (args.size() < 2) {
            return fail(QStringLiteral("usage: patchprime-ctl pad <name>"));
        }
        const auto wanted = args.mid(1).join(QLatin1Char(' '));
        for (const auto &p : state.value(QLatin1String("pads")).toArray()) {
            const auto pad = p.toObject();
            if (pad.value(QLatin1String("label")).toString().compare(wanted, Qt::CaseInsensitive) == 0 ||
                pad.value(QLatin1String("id")).toString() == wanted) {
                return Plan{.calls = {{.method = QStringLiteral("TriggerPad"),
                                       .args = {pad.value(QLatin1String("id")).toString()},
                                       .returnsError = true}},
                            .output = {}};
            }
        }
        return fail(QStringLiteral("no pad called \"%1\" (see: patchprime-ctl pads)").arg(wanted));
    }

    // Everything else acts on a strip.
    static const QStringList stripCommands{QStringLiteral("volume"), QStringLiteral("mute"), QStringLiteral("cue"),
                                           QStringLiteral("send"),   QStringLiteral("send-level"),
                                           QStringLiteral("effect"), QStringLiteral("record")};
    if (!stripCommands.contains(command)) {
        return fail(QStringLiteral("unknown command \"%1\" (see: patchprime-ctl --help)").arg(args.front()));
    }
    if (args.size() < 2) {
        return fail(QStringLiteral("\"%1\" needs a strip (see: patchprime-ctl --help)").arg(command));
    }
    const auto strip = findStrip(state, args.at(1));
    if (!strip) {
        return fail(strip.error());
    }
    if (command == QLatin1String("volume")) {
        const auto db = level(args, 2, strip->value(QLatin1String("volumeDb")).toDouble());
        if (!db) {
            return fail(db.error());
        }
        return Plan{.calls = {{.method = QStringLiteral("SetVolume"), .args = {key(*strip), *db}}}, .output = {}};
    }
    if (command == QLatin1String("mute") || command == QLatin1String("cue")) {
        const bool mute = command == QLatin1String("mute");
        const auto on = onOff(args, 2, strip->value(QLatin1String(mute ? "muted" : "cued")).toBool());
        if (!on) {
            return fail(on.error());
        }
        if (!mute && state.value(QLatin1String("cueOutput")).toString().isEmpty()) {
            return fail(QStringLiteral("there's no cue output (choose one in the mixer)"));
        }
        return Plan{.calls = {{.method = mute ? QStringLiteral("SetMuted") : QStringLiteral("SetCue"), .args = {key(*strip), *on}}},
                    .output = {}};
    }
    if (command == QLatin1String("send") || command == QLatin1String("send-level")) {
        if (args.size() < 3) {
            return fail(QStringLiteral("\"%1\" needs a strip to send to").arg(command));
        }
        const auto to = findStrip(state, args.at(2));
        if (!to) {
            return fail(to.error());
        }
        const bool sending = sendsTo(*strip, key(*to));
        if (command == QLatin1String("send")) {
            const auto on = onOff(args, 3, sending);
            if (!on) {
                return fail(on.error());
            }
            return Plan{.calls = {{.method = QStringLiteral("SetSend"), .args = {key(*strip), key(*to), *on}}},
                        .output = {}};
        }
        if (!sending) {
            return fail(QStringLiteral("%1 isn't sent to %2 (turn the send on first)")
                            .arg(strip->value(QLatin1String("label")).toString(),
                                 to->value(QLatin1String("label")).toString()));
        }
        const double current = strip->value(QLatin1String("sendLevels")).toObject().value(key(*to)).toDouble(0.0);
        const auto db = level(args, 3, current);
        if (!db) {
            return fail(db.error());
        }
        return Plan{.calls = {{.method = QStringLiteral("SetSendLevel"), .args = {key(*strip), key(*to), *db}}},
                    .output = {}};
    }
    if (command == QLatin1String("effect")) {
        const auto effects = strip->value(QLatin1String("effects")).toArray();
        bool ok = false;
        const int n = args.size() > 2 ? args.at(2).toInt(&ok) : 0;
        if (!ok || n < 1 || n > effects.size()) {
            return fail(QStringLiteral("%1 has %2 effect(s): give a number from 1")
                            .arg(strip->value(QLatin1String("label")).toString())
                            .arg(effects.size()));
        }
        const auto on = onOff(args, 3, effects.at(n - 1).toObject().value(QLatin1String("enabled")).toBool());
        if (!on) {
            return fail(on.error());
        }
        return Plan{.calls = {{.method = QStringLiteral("SetEffectEnabled"), .args = {key(*strip), n - 1, *on}}},
                    .output = {}};
    }
    if (command == QLatin1String("record")) {
        const bool recording = state.value(QLatin1String("recording")).toObject().contains(key(*strip));
        const auto on = onOff(args, 2, recording);
        if (!on) {
            return fail(on.error());
        }
        if (*on == recording) {
            return Plan{};
        }
        return Plan{.calls = {{.method = *on ? QStringLiteral("StartRecording") : QStringLiteral("StopRecording"),
                               .args = {key(*strip)},
                               .returnsError = *on}},
                    .output = {}};
    }
    return fail(QStringLiteral("unknown command \"%1\" (see: patchprime-ctl --help)").arg(args.front()));
}

} // namespace pp::ctl

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <expected>
#include <vector>

// patchprime-ctl: commands for scripts and Stream Deck buttons, turned into
// calls to the daemon's D-Bus API (org.kingsqueak.PatchPrime1). Planning
// is separate from calling, so it can be tested without a daemon.
namespace pp::ctl {

struct Call
{
    QString method;
    QVariantList args;
    // Methods that answer with an error message ("" when it worked).
    bool returnsError = false;
};

struct Plan
{
    std::vector<Call> calls;
    QString output; // printed (list, get, scenes)
};

// args: the command line after the program name. state: GetState's JSON.
// The error is for people.
std::expected<Plan, QString> plan(const QStringList &args, const QJsonObject &state);
QString usage();

} // namespace pp::ctl

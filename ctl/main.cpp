// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "ctl.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonDocument>

#include <cstdio>
#include <exception>
#include <print>

namespace {

const auto service = QStringLiteral("org.kingsqueak.PatchPrime");
const auto path = QStringLiteral("/org/kingsqueak/PatchPrime");
const auto interface = QStringLiteral("org.kingsqueak.PatchPrime1");

QDBusMessage call(const QString &method, const QVariantList &args = {})
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(args);
    return QDBusConnection::sessionBus().call(message);
}

int run(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    auto args = QCoreApplication::arguments().mid(1);
    if (args.isEmpty() || args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        std::print("{}", pp::ctl::usage().toStdString());
        return args.isEmpty() ? 2 : 0;
    }
    if (args.contains(QStringLiteral("--version"))) {
        std::println("patchprime-ctl {}", PATCHPRIME_VERSION);
        return 0;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        std::println(stderr, "patchprime-ctl: no D-Bus session bus");
        return 1;
    }
    // Starts the engine through D-Bus activation when it isn't running.
    const auto state = call(QStringLiteral("GetState"));
    if (state.type() != QDBusMessage::ReplyMessage || state.arguments().isEmpty()) {
        std::println(stderr, "patchprime-ctl: Patch Prime's engine doesn't answer ({})",
                     state.errorMessage().toStdString());
        return 1;
    }
    const auto json = QJsonDocument::fromJson(state.arguments().constFirst().toString().toUtf8()).object();
    const auto plan = pp::ctl::plan(args, json);
    if (!plan) {
        std::println(stderr, "patchprime-ctl: {}", plan.error().toStdString());
        return 2;
    }
    for (const auto &c : plan->calls) {
        const auto reply = call(c.method, c.args);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            std::println(stderr, "patchprime-ctl: {}", reply.errorMessage().toStdString());
            return 1;
        }
        if (c.returnsError && !reply.arguments().isEmpty() && !reply.arguments().constFirst().toString().isEmpty()) {
            std::println(stderr, "patchprime-ctl: {}", reply.arguments().constFirst().toString().toStdString());
            return 1;
        }
    }
    std::print("{}", plan->output.toStdString());
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::fputs("patchprime-ctl: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}

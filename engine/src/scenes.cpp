// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "scenes.h"

#include "approuter.h"
#include "appstore.h"
#include "busmanager.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "pipewiregraph.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace pp {

namespace {

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

} // namespace

QJsonObject captureScene(const QString &name, const DeviceStore &devices, const AppStore &apps,
                         const std::string &defaultOutput, const std::string &defaultInput)
{
    QJsonObject strips;
    for (const auto &[key, settings] : devices.entries()) {
        if (settings.enabled) {
            strips.insert(qs(key), mixToJson(settings));
        }
    }
    QJsonObject routes;
    for (const auto &[key, app] : apps.entries()) {
        routes.insert(qs(key), qs(app.target));
    }
    return {
        {QStringLiteral("version"), sceneVersion},
        {QStringLiteral("name"), name},
        {QStringLiteral("saved"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("strips"), strips},
        {QStringLiteral("apps"), routes},
        {QStringLiteral("defaults"), QJsonObject{{QStringLiteral("output"), qs(defaultOutput)},
                                                 {QStringLiteral("input"), qs(defaultInput)}}},
    };
}

int applyScene(const QJsonObject &scene, DeviceModel &devices, AppRouter *apps,
               PipeWireGraph *graph)
{
    int applied = 0;
    const auto strips = scene.value(QLatin1String("strips")).toObject();
    for (auto it = strips.begin(); it != strips.end(); ++it) {
        if (devices.applyMix(it.key().toStdString(), it.value().toObject())) {
            ++applied;
        }
    }
    if (apps != nullptr) {
        const auto routes = scene.value(QLatin1String("apps")).toObject();
        for (auto it = routes.begin(); it != routes.end(); ++it) {
            apps->setTarget(it.key().toStdString(), it.value().toString().toStdString());
        }
    }
    if (graph != nullptr) {
        const auto defaults = scene.value(QLatin1String("defaults")).toObject();
        auto exists = [graph](const std::string &name) {
            return std::ranges::any_of(graph->nodes(), [&name](const auto &entry) {
                return prop(entry.second, "node.name") == name;
            });
        };
        for (const auto &[field, direction] :
             {std::pair{"output", Direction::Output}, std::pair{"input", Direction::Input}}) {
            const auto name = defaults.value(QLatin1String(field)).toString().toStdString();
            if (!name.empty() && exists(name)) {
                graph->setConfiguredDefault(direction, name);
            }
        }
    }
    return applied;
}

SceneStore::SceneStore(QString dir) : m_dir(dir.isEmpty() ? defaultDir() : std::move(dir)) {}

QString SceneStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/patchprime/scenes");
}

QString SceneStore::fileFor(const QString &name) const
{
    // The file name only needs to be stable and safe; the scene's own name
    // is kept inside. (Not qHash: it's seeded differently in every process.)
    const auto trimmed = name.trimmed();
    const auto hash =
        QCryptographicHash::hash(trimmed.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    return m_dir + QLatin1Char('/') + QString::fromStdString(nameSlug(trimmed.toStdString())) +
           QLatin1Char('-') + QString::fromLatin1(hash) + QStringLiteral(".json");
}

QStringList SceneStore::names() const
{
    QStringList out;
    for (const auto &file : QDir(m_dir).entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(m_dir + QLatin1Char('/') + file);
        if (f.open(QIODevice::ReadOnly)) {
            const auto name = QJsonDocument::fromJson(f.readAll())
                                  .object()
                                  .value(QLatin1String("name"))
                                  .toString();
            if (!name.isEmpty()) {
                out.append(name);
            }
        }
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

bool SceneStore::save(const QJsonObject &scene)
{
    const auto name = scene.value(QLatin1String("name")).toString().trimmed();
    if (name.isEmpty() || !QDir().mkpath(m_dir)) {
        return false;
    }
    QSaveFile file(fileFor(name));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    auto stored = scene;
    stored.insert(QStringLiteral("name"), name);
    file.write(QJsonDocument(stored).toJson(QJsonDocument::Indented));
    return file.commit();
}

QJsonObject SceneStore::load(const QString &name) const
{
    QFile file(fileFor(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool SceneStore::remove(const QString &name)
{
    return QFile::remove(fileFor(name));
}

} // namespace pp

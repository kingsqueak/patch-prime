// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "appstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace pp {

namespace {

constexpr int formatVersion = 1;

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString().toStdString();
}

} // namespace

std::string streamDirectionName(StreamDirection direction)
{
    return direction == StreamDirection::Record ? "record" : "playback";
}

StreamDirection streamDirectionFromName(std::string_view name)
{
    return name == "record" ? StreamDirection::Record : StreamDirection::Playback;
}

AppStore::AppStore(QString path) : m_path(path.isEmpty() ? defaultPath() : std::move(path)) {}

QString AppStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/patchprime/apps.json");
}

bool AppStore::load()
{
    m_entries.clear();
    m_lastError.clear();
    QFile file(m_path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        m_lastError = parseError.errorString();
        return false;
    }
    const auto apps = document.object().value(QLatin1String("apps")).toObject();
    for (auto it = apps.begin(); it != apps.end(); ++it) {
        const auto o = it.value().toObject();
        m_entries[it.key().toStdString()] = AppSettings{
            .direction = streamDirectionFromName(str(o, "direction")),
            .name = str(o, "name"),
            .icon = str(o, "icon"),
            .target = str(o, "target"),
            .lastSeen =
                QDateTime::fromString(o.value(QLatin1String("lastSeen")).toString(), Qt::ISODate),
            .hidden = o.value(QLatin1String("hidden")).toBool(),
        };
    }
    return true;
}

bool AppStore::save()
{
    m_lastError.clear();
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_lastError = QStringLiteral("could not create ") + QFileInfo(m_path).absolutePath();
        return false;
    }
    QJsonObject apps;
    for (const auto &[key, a] : m_entries) {
        apps.insert(qs(key),
                    QJsonObject{
                        {QStringLiteral("direction"), qs(streamDirectionName(a.direction))},
                        {QStringLiteral("name"), qs(a.name)},
                        {QStringLiteral("icon"), qs(a.icon)},
                        {QStringLiteral("target"), qs(a.target)},
                        {QStringLiteral("lastSeen"), a.lastSeen.toString(Qt::ISODate)},
                        {QStringLiteral("hidden"), a.hidden},
                    });
    }
    const QJsonObject root{
        {QStringLiteral("version"), formatVersion},
        {QStringLiteral("apps"), apps},
    };

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
}

const AppSettings *AppStore::find(const std::string &key) const
{
    const auto it = m_entries.find(key);
    return it != m_entries.end() ? &it->second : nullptr;
}

} // namespace pp

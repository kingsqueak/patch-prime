// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicestore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
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

QJsonObject toJson(const DeviceSettings &d)
{
    return QJsonObject{
        {QStringLiteral("enabled"), d.enabled},
        {QStringLiteral("label"), qs(d.label)},
        {QStringLiteral("nodeName"), qs(d.nodeName)},
        {QStringLiteral("mediaClass"), qs(d.mediaClass)},
        {QStringLiteral("direction"),
         d.direction == Direction::Input ? QStringLiteral("input") : QStringLiteral("output")},
        {QStringLiteral("virtual"), d.isVirtual},
        {QStringLiteral("description"), qs(d.description)},
        {QStringLiteral("channels"), d.channels},
        {QStringLiteral("lastSeen"), d.lastSeen.toString(Qt::ISODate)},
        {QStringLiteral("signature"),
         QJsonObject{
             {QStringLiteral("serial"), qs(d.signature.deviceSerial)},
             {QStringLiteral("vendorId"), qs(d.signature.vendorId)},
             {QStringLiteral("productId"), qs(d.signature.productId)},
             {QStringLiteral("busPath"), qs(d.signature.busPath)},
             {QStringLiteral("profile"), qs(d.signature.profile)},
         }},
    };
}

DeviceSettings fromJson(const QJsonObject &o)
{
    DeviceSettings d;
    d.enabled = o.value(QLatin1String("enabled")).toBool();
    d.label = str(o, "label");
    d.nodeName = str(o, "nodeName");
    d.mediaClass = str(o, "mediaClass");
    d.direction = str(o, "direction") == "input" ? Direction::Input : Direction::Output;
    d.isVirtual = o.value(QLatin1String("virtual")).toBool();
    d.description = str(o, "description");
    d.channels = o.value(QLatin1String("channels")).toInt();
    d.lastSeen = QDateTime::fromString(o.value(QLatin1String("lastSeen")).toString(), Qt::ISODate);
    const auto sig = o.value(QLatin1String("signature")).toObject();
    d.signature.deviceSerial = str(sig, "serial");
    d.signature.vendorId = str(sig, "vendorId");
    d.signature.productId = str(sig, "productId");
    d.signature.busPath = str(sig, "busPath");
    d.signature.profile = str(sig, "profile");
    return d;
}

} // namespace

DeviceStore::DeviceStore(QString path) : m_path(path.isEmpty() ? defaultPath() : std::move(path)) {}

QString DeviceStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/patchprime/devices.json");
}

bool DeviceStore::load()
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
    const auto devices = document.object().value(QLatin1String("devices")).toObject();
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        m_entries[it.key().toStdString()] = fromJson(it.value().toObject());
    }
    return true;
}

bool DeviceStore::save()
{
    m_lastError.clear();
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_lastError = QStringLiteral("could not create ") + QFileInfo(m_path).absolutePath();
        return false;
    }
    QJsonObject devices;
    for (const auto &[key, settings] : m_entries) {
        devices.insert(qs(key), toJson(settings));
    }
    const QJsonObject root{
        {QStringLiteral("version"), formatVersion},
        {QStringLiteral("devices"), devices},
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

const DeviceSettings *DeviceStore::find(const std::string &key) const
{
    const auto it = m_entries.find(key);
    return it != m_entries.end() ? &it->second : nullptr;
}

DeviceSettings &DeviceStore::upsert(const std::string &key)
{
    return m_entries[key];
}

bool DeviceStore::remove(const std::string &key)
{
    return m_entries.erase(key) > 0;
}

bool DeviceStore::rekey(const std::string &from, const std::string &to)
{
    auto node = m_entries.extract(from);
    if (node.empty() || m_entries.contains(to)) {
        if (!node.empty()) {
            m_entries.insert(std::move(node));
        }
        return false;
    }
    node.key() = to;
    m_entries.insert(std::move(node));
    return true;
}

} // namespace pp

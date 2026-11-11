// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "updatechecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

constexpr int timeoutMs = 10000;
constexpr auto dismissedKey = "Updates/dismissed"_L1;

QString withoutV(QString version)
{
    version = version.trimmed();
    if (version.startsWith(u'v', Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    return version;
}

QList<int> parts(const QString &version)
{
    QList<int> out;
    for (const QString &part : withoutV(version).split(u'.')) {
        qsizetype digits = 0;
        while (digits < part.size() && part.at(digits).isDigit()) {
            ++digits;
        }
        out.append(part.left(digits).toInt());
    }
    return out;
}

} // namespace

UpdateChecker::UpdateChecker(QString url, int startDelayMs, QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_url(std::move(url)),
      m_dismissed(QSettings().value(dismissedKey).toString())
{
    if (const auto test = qEnvironmentVariable("PATCHPRIME_UPDATE_URL"); !test.isEmpty()) {
        m_url = test;
    }
    if (startDelayMs >= 0) {
        m_startTimer.setSingleShot(true);
        m_startTimer.setInterval(startDelayMs);
        connect(&m_startTimer, &QTimer::timeout, this, &UpdateChecker::check);
        m_startTimer.start();
    }
}

QString UpdateChecker::current()
{
    return QCoreApplication::applicationVersion();
}

bool UpdateChecker::available() const
{
    return !m_latest.isEmpty() && isNewer(m_latest, current());
}

bool UpdateChecker::showNotice() const
{
    return available() && m_latest != m_dismissed;
}

void UpdateChecker::check()
{
    if (m_checking) {
        return;
    }
    m_checking = true;
    emit changed();
    QNetworkRequest request{QUrl(m_url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, u"PatchPrime/"_s + current());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_checking = false;
        const QJsonObject root =
            reply->error() == QNetworkReply::NoError ? QJsonDocument::fromJson(reply->readAll()).object() : QJsonObject();
        const QString tag = withoutV(root.value("tag_name"_L1).toString());
        m_failed = tag.isEmpty();
        if (!m_failed) {
            m_latest = tag;
            m_page = root.value("html_url"_L1).toString();
            if (m_page.isEmpty()) {
                m_page = QString::fromLatin1(releasesPage);
            }
        }
        emit changed();
        emit checked();
    });
}

void UpdateChecker::dismiss()
{
    if (m_latest.isEmpty() || m_dismissed == m_latest) {
        return;
    }
    m_dismissed = m_latest;
    QSettings().setValue(dismissedKey, m_dismissed);
    emit changed();
}

bool UpdateChecker::isNewer(const QString &candidate, const QString &current)
{
    const QList<int> a = parts(candidate);
    const QList<int> b = parts(current);
    if (withoutV(candidate).isEmpty()) {
        return false;
    }
    for (qsizetype i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a.at(i) : 0;
        const int y = i < b.size() ? b.at(i) : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}

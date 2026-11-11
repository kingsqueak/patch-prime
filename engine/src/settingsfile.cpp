// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "settingsfile.h"

#include <QDateTime>
#include <QFile>

namespace pp {

QString setAsideUnreadable(const QString &path)
{
    const QString base =
        path + QStringLiteral(".unreadable-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString target = base;
    for (int n = 2; QFile::exists(target); ++n) {
        target = base + QLatin1Char('-') + QString::number(n);
    }
    return QFile::rename(path, target) ? target : QString();
}

} // namespace pp

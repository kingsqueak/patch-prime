// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace pp {

// The user's language, if there's a translation: Patch Prime's own (built
// in) and Qt's, for its standard buttons (installed with Qt's translations,
// if at all). English is the source text, so it needs nothing.
inline void loadTranslations(QCoreApplication &app)
{
    // QTranslator goes down the user's list of languages, and with no English
    // file it would pass over English to a second choice.
    if (QLocale().uiLanguages().value(0).startsWith(QLatin1String("en"))) {
        return;
    }
    auto *own = new QTranslator(&app);
    if (own->load(QLocale(), QStringLiteral("patchprime"), QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(own);
    }
    auto *qt = new QTranslator(&app);
    if (qt->load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qt);
    }
}

} // namespace pp

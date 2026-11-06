// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "effects.h"
#include "effectstrings.h"
#include "presets.h"

#include <QTest>
#include <QTranslator>

using namespace pp;

namespace {

// Stands in for a language: every string gets brackets.
class Brackets : public QTranslator
{
public:
    [[nodiscard]] bool isEmpty() const override { return false; }
    [[nodiscard]] QString translate(const char *context, const char *source, const char * /*disambiguation*/,
                                    int /*n*/) const override
    {
        if (QLatin1String(context) != QLatin1String("Effects")) {
            return {};
        }
        return QStringLiteral("[%1]").arg(QLatin1String(source));
    }
};

} // namespace

class EffectStringsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Everything the engine can say about an effect is in the list: a new
    // effect or parameter that fails here needs a line in effectstrings.cpp.
    void listIsComplete()
    {
        QStringList missing;
        const auto check = [&missing](const std::string &text) {
            if (const auto s = QString::fromStdString(text); !s.isEmpty() && !isEffectText(s) && !missing.contains(s)) {
                missing.append(s);
            }
        };
        for (const auto &type : effectTypes()) {
            check(effectLabel(type));
            for (const auto &p : effectParams(type)) {
                check(p.label);
                std::ranges::for_each(p.options, check);
            }
        }
        for (const auto &name : PresetStore::voiceChainNames()) {
            check(name.toStdString());
        }
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral(", "))));
    }

    void translates()
    {
        Brackets language;
        QCoreApplication::installTranslator(&language);
        QCOMPARE(effectText(QStringLiteral("Reverb")), QStringLiteral("[Reverb]"));
        QCOMPARE(effectText(QStringLiteral("Band 3 gain")), QStringLiteral("[Band 3 [gain]]"));
        // A plugin's own words stay as they are.
        QCOMPARE(effectText(QStringLiteral("Tube Warmth")), QStringLiteral("Tube Warmth"));
        QCOMPARE(effectText(QStringLiteral("Band 3 warmth")), QStringLiteral("Band 3 warmth"));
        QCoreApplication::removeTranslator(&language);
    }
};

QTEST_GUILESS_MAIN(EffectStringsTest)
#include "test_effectstrings.moc"

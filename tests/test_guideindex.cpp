// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "guideindex.h"

#include <QFile>
#include <QSet>
#include <QTest>

using namespace Qt::StringLiterals;
using pp::GuideTopic;

// The parser on a small guide, and the real guide: its links, its anchors and
// that the words people look for find the right topic.
class TestGuideIndex : public QObject
{
    Q_OBJECT

private:
    QString m_guide;
    QList<GuideTopic> m_topics;

    [[nodiscard]] QString firstHit(const QString &query) const
    {
        const auto hits = pp::searchGuide(m_topics, query);
        return hits.isEmpty() ? QString() : m_topics[hits.first().topic].title;
    }

private slots:
    void initTestCase()
    {
        QFile file(QStringLiteral(PATCHPRIME_GUIDE_FILE));
        QVERIFY(file.open(QIODevice::ReadOnly));
        m_guide = QString::fromUtf8(file.readAll());
        m_topics = pp::parseGuide(m_guide);
        QVERIFY(m_topics.size() > 50);
    }

    void anchorsAreGitHubs()
    {
        QCOMPARE(pp::guideAnchor(u"Cue (headphones)"_s), u"cue-headphones"_s);
        QCOMPARE(pp::guideAnchor(u"Plugins (LV2, CLAP and VST3)"_s), u"plugins-lv2-clap-and-vst3"_s);
        QCOMPARE(pp::guideAnchor(u"Setting up a mic's effects on a recording"_s),
                 u"setting-up-a-mics-effects-on-a-recording"_s);
        QCOMPARE(pp::guideAnchor(u"De-esser"_s), u"de-esser"_s);
    }

    void parsesLevels()
    {
        const auto topics = pp::parseGuide(u"# Guide\n\nHello.\n\n## Mixing\n\nAbout mixing.\n\n"
                                           "### Faders\n<!-- keywords: Level, gain -->\n\nMove them.\n\n"
                                           "#### Fine moves\n\nSlowly.\n\n"
                                           "```\n# not a heading\n### nor this\n```\n\n"
                                           "## Empty\n"_s);
        QCOMPARE(topics.size(), 4);
        QCOMPARE(topics[0].level, 1);
        QCOMPARE(topics[1].group, u"Mixing"_s);
        QVERIFY(topics[1].markdown.endsWith(u"- [Faders](#faders)"_s));
        QVERIFY2(!topics[1].plain.contains(u"Faders"_s), "a group's list isn't its text");
        QCOMPARE(topics[2].group, u"Mixing"_s);
        QCOMPARE(topics[2].keywords, u"level, gain"_s);
        QVERIFY(!topics[2].markdown.contains(u"keywords"_s));
        QVERIFY(topics[2].markdown.contains(u"# not a heading"_s));
        QCOMPARE(topics[2].innerAnchors, QStringList{u"fine-moves"_s});
        QCOMPARE(pp::guideTopicForAnchor(topics, u"fine-moves"_s), 2);
        QCOMPARE(pp::guideTopicForAnchor(topics, u"missing"_s), -1);
    }

    void plainTextHasNoMarkup()
    {
        const auto topics = pp::parseGuide(u"# T\n\n**Bold**, `code` and [a link](#t).\n\n"
                                           "| A | B |\n|---|---|\n| one | two |\n"_s);
        QCOMPARE(topics[0].plain, u"Bold, code and a link. A B one two"_s);
    }

    void everyAnchorIsUnique()
    {
        QSet<QString> seen;
        for (const auto &topic : std::as_const(m_topics)) {
            QVERIFY2(!seen.contains(topic.anchor), qPrintable(topic.anchor));
            seen.insert(topic.anchor);
        }
    }

    void everyLinkLeadsSomewhere()
    {
        for (const auto &anchor : pp::guideLinks(m_guide)) {
            QVERIFY2(pp::guideTopicForAnchor(m_topics, anchor) >= 0, qPrintable(anchor));
        }
    }

    void everyTopicIsInTheContents()
    {
        const QStringList links = pp::guideLinks(m_topics.first().markdown);
        for (const auto &topic : std::as_const(m_topics)) {
            if (topic.level > 1) {
                QVERIFY2(links.contains(topic.anchor), qPrintable(topic.anchor));
            }
        }
    }

    void everyTopicSaysSomething()
    {
        for (const auto &topic : std::as_const(m_topics)) {
            QVERIFY2(topic.plain.size() > 10, qPrintable(topic.title));
        }
    }

    void findsTheRightTopic_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("topic");
        QTest::newRow("a word it doesn't use in its title") << u"solo"_s << u"Cue (headphones)"_s;
        QTest::newRow("a keyword") << u"sidechain"_s << u"Ducker"_s;
        QTest::newRow("any case") << u"RNNOISE"_s << u"Noise suppression"_s;
        QTest::newRow("a phrase") << u"push to talk"_s << u"Global hotkeys"_s;
        QTest::newRow("two words apart") << u"limiter true peak"_s << u"Safety limiter"_s;
        QTest::newRow("the start of a word") << u"compres"_s << u"Compressor"_s;
        QTest::newRow("a title") << u"mini window"_s << u"Mini window"_s;
        QTest::newRow("a command") << u"patchprime-ctl"_s << u"Scripts and Stream Deck (patchprime-ctl)"_s;
        QTest::newRow("a problem") << u"no sound"_s << u"No sound from a strip"_s;
        QTest::newRow("a shortcut") << u"ctrl+m"_s << u"Mini window"_s;
    }
    void findsTheRightTopic()
    {
        QFETCH(QString, query);
        QFETCH(QString, topic);
        QCOMPARE(firstHit(query), topic);
    }

    void needsEveryWord()
    {
        QVERIFY(pp::searchGuide(m_topics, u"compressor xyzzy"_s).isEmpty());
        QVERIFY(pp::searchGuide(m_topics, u"   "_s).isEmpty());
    }

    void limitsResults()
    {
        QCOMPARE(pp::searchGuide(m_topics, u"the"_s, 5).size(), 5);
    }

    void snippetsAreEscapedAndMarked()
    {
        const auto topics = pp::parseGuide(u"# T\n\n## G\n\nIntro here.\n\n### A\n\nUse a <b> & the fader now.\n"_s);
        const auto hits = pp::searchGuide(topics, u"fader"_s);
        QCOMPARE(hits.size(), 1);
        QVERIFY(hits[0].snippet.contains(u"<b>fader</b>"_s));
        QVERIFY(hits[0].snippet.contains(u"&lt;b&gt; &amp;"_s));
    }

    void snippetsShowThePhraseAndWholeWords()
    {
        const auto topics = pp::parseGuide(u"# T\n\n## G\n\nIntro here.\n\n### A\n\n"
                                           "A button to press. Then push to talk works.\n"_s);
        const auto hits = pp::searchGuide(topics, u"push to"_s);
        QCOMPARE(hits.size(), 1);
        QVERIFY(hits[0].snippet.contains(u"<b>push</b> <b>to</b>"_s));
        QVERIFY2(!hits[0].snippet.contains(u"bu<b>"_s), qPrintable(hits[0].snippet));
    }

    void aKeywordOnlyMatchStillHasASnippet()
    {
        const auto topics = pp::parseGuide(u"# T\n\n## G\n\nIntro.\n\n### A\n<!-- keywords: zebra -->\n\nPlain words.\n"_s);
        const auto hits = pp::searchGuide(topics, u"zebra"_s);
        QCOMPARE(hits.size(), 1);
        QVERIFY(hits[0].snippet.contains(u"Plain words"_s));
    }
};

QTEST_GUILESS_MAIN(TestGuideIndex)
#include "test_guideindex.moc"

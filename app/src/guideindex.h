// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace pp {

// One page of the user guide. The guide is a single Markdown file (it reads
// as one on GitHub); the app shows it a topic at a time:
//   #    the home page
//   ##   a group: its introduction, then a list of its topics
//   ###  a topic, with any #### headings inside it
// A line <!-- keywords: solo, pfl --> under a heading gives words to find the
// topic by that it needn't use itself. It doesn't show on any page.
struct GuideTopic {
    QString title;
    QString anchor;  // as GitHub makes it, so links work in both places
    QString group;   // the group's title; empty on the home page
    int level = 1;   // of its heading: 1, 2 or 3
    QString markdown;
    QString plain;   // the text as read, for searching
    QString keywords;
    QStringList innerAnchors; // of the #### headings inside it
};

struct GuideHit {
    int topic = -1;
    int score = 0;
    QString snippet; // HTML: the text around a match, the matches in bold
};

[[nodiscard]] QString guideAnchor(const QString &heading);
[[nodiscard]] QList<GuideTopic> parseGuide(const QString &markdown);
// The topic with this anchor, or a #### inside it; -1 if there's none.
[[nodiscard]] int guideTopicForAnchor(const QList<GuideTopic> &topics, const QString &anchor);
// Every internal link (#anchor) in the guide, for checking them.
[[nodiscard]] QStringList guideLinks(const QString &markdown);
// The words of a search, lower case.
[[nodiscard]] QStringList guideSearchTerms(const QString &query);
// Topics with every word of the query, best first.
[[nodiscard]] QList<GuideHit> searchGuide(const QList<GuideTopic> &topics, const QString &query,
                                          int limit = 40);

} // namespace pp

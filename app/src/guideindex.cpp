// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "guideindex.h"

#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace pp {

namespace {

constexpr int snippetBefore = 40;
constexpr int snippetLength = 130;
constexpr int bodyCountLimit = 10; // a long topic shouldn't win on length alone

bool isFence(const QString &line)
{
    return line.startsWith("```"_L1);
}

QString plainText(const QString &markdown)
{
    static const QRegularExpression link(uR"(\[([^\]]*)\]\([^)]*\))"_s);
    static const QRegularExpression rule(uR"(^\s*\|?[\s:|-]*-{3,}[\s:|-]*$)"_s);
    static const QRegularExpression marks(uR"(\*\*|\*|`)"_s);
    static const QRegularExpression heading(uR"(^#{1,6}\s+)"_s);
    static const QRegularExpression list(uR"(^\s*(?:[-*]|\d+\.)\s+)"_s);
    QStringList lines;
    for (QString line : markdown.split(u'\n')) {
        if (isFence(line) || rule.match(line).hasMatch()) {
            continue;
        }
        line.replace(link, u"\\1"_s).remove(marks).remove(heading).remove(list);
        line.replace(u'|', u' ');
        lines.append(line.simplified());
    }
    return lines.join(u' ').simplified();
}

bool wordStartsAt(const QString &text, qsizetype at)
{
    return at == 0 || !text.at(at - 1).isLetterOrNumber();
}

bool wordEndsAt(const QString &text, qsizetype at)
{
    return at >= text.size() || !text.at(at).isLetterOrNumber();
}

// How well a word fits a short text: whole word, start of a word, or anywhere.
int fit(const QString &text, const QString &term, int whole, int start, int inside)
{
    int best = 0;
    for (qsizetype at = text.indexOf(term); at >= 0; at = text.indexOf(term, at + 1)) {
        if (wordStartsAt(text, at)) {
            best = std::max(best, wordEndsAt(text, at + term.size()) ? whole : start);
        } else {
            best = std::max(best, inside);
        }
    }
    return best;
}

int countIn(const QString &text, const QString &term)
{
    int found = 0;
    for (qsizetype at = text.indexOf(term); at >= 0 && found < bodyCountLimit;
         at = text.indexOf(term, at + term.size())) {
        ++found;
    }
    return found;
}

QString snippet(const QString &plain, const QStringList &terms)
{
    const QString lower = plain.toLower();
    // Where the words stand together, else where the first of them starts a word.
    qsizetype first = lower.indexOf(terms.join(u' '));
    if (first < 0) {
        for (const auto &term : terms) {
            qsizetype at = lower.indexOf(term);
            while (at >= 0 && !wordStartsAt(lower, at)) {
                at = lower.indexOf(term, at + 1);
            }
            if (at >= 0 && (first < 0 || at < first)) {
                first = at;
            }
        }
    }
    qsizetype start = first < 0 ? 0 : std::max<qsizetype>(0, first - snippetBefore);
    // Start on a word.
    if (start > 0) {
        const qsizetype space = plain.indexOf(u' ', start);
        if (space >= 0 && space < first) {
            start = space + 1;
        }
    }
    const QString cut = plain.mid(start, snippetLength);
    const QString cutLower = cut.toLower();

    // Mark the matches, then escape the pieces between them.
    QList<bool> marked(cut.size(), false);
    for (const auto &term : terms) {
        for (qsizetype at = cutLower.indexOf(term); at >= 0; at = cutLower.indexOf(term, at + term.size())) {
            if (wordStartsAt(cutLower, at)) {
                std::fill_n(marked.begin() + at, std::min(term.size(), cut.size() - at), true);
            }
        }
    }
    QString html = start > 0 ? u"…"_s : QString();
    for (qsizetype i = 0; i < cut.size();) {
        qsizetype end = i;
        while (end < cut.size() && marked[end] == marked[i]) {
            ++end;
        }
        const QString piece = cut.mid(i, end - i).toHtmlEscaped();
        html += marked[i] ? "<b>"_L1 + piece + "</b>"_L1 : piece;
        i = end;
    }
    if (start + cut.size() < plain.size()) {
        html += u'…';
    }
    return html;
}

} // namespace

QString guideAnchor(const QString &heading)
{
    QString anchor;
    for (const QChar c : heading.trimmed().toLower()) {
        if (c.isLetterOrNumber() || c == u'-' || c == u'_') {
            anchor += c;
        } else if (c.isSpace()) {
            anchor += u'-';
        }
    }
    return anchor;
}

QList<GuideTopic> parseGuide(const QString &markdown)
{
    static const QRegularExpression headingLine(uR"(^(#{1,4})\s+(.*\S)\s*$)"_s);
    static const QRegularExpression keywordsLine(uR"(^<!--\s*keywords:\s*(.*?)\s*-->\s*$)"_s);

    QList<GuideTopic> topics;
    QString group;
    bool inFence = false;
    for (const QString &line : markdown.split(u'\n')) {
        if (isFence(line)) {
            inFence = !inFence;
        }
        const auto heading = inFence ? QRegularExpressionMatch() : headingLine.match(line);
        const int level = heading.hasMatch() ? static_cast<int>(heading.capturedLength(1)) : 0;
        if (level >= 1 && level <= 3) {
            GuideTopic topic;
            topic.title = heading.captured(2);
            topic.anchor = guideAnchor(topic.title);
            topic.level = level;
            if (level == 2) {
                group = topic.title;
            }
            topic.group = group;
            topics.append(topic);
        } else if (topics.isEmpty()) {
            continue; // nothing comes before the title
        } else if (const auto keywords = inFence ? QRegularExpressionMatch() : keywordsLine.match(line);
                   keywords.hasMatch()) {
            topics.last().keywords = keywords.captured(1).toLower();
            continue;
        } else if (level == 4) {
            topics.last().innerAnchors.append(guideAnchor(heading.captured(2)));
        }
        topics.last().markdown += line + u'\n';
    }

    for (auto &topic : topics) {
        topic.markdown = topic.markdown.trimmed();
        // The title is searched on its own, and snippets read better without it.
        topic.plain = plainText(topic.markdown.section(u'\n', 1));
    }
    // A group's page ends with its topics. This comes after plain: the words
    // of the list belong to the topics, not to the group.
    for (qsizetype i = 0; i < topics.size(); ++i) {
        if (topics[i].level != 2) {
            continue;
        }
        QString list;
        for (qsizetype t = i + 1; t < topics.size() && topics[t].level == 3; ++t) {
            list += "- ["_L1 + topics[t].title + "](#"_L1 + topics[t].anchor + ")\n"_L1;
        }
        if (!list.isEmpty()) {
            topics[i].markdown += "\n\n"_L1 + list.trimmed();
        }
    }
    return topics;
}

int guideTopicForAnchor(const QList<GuideTopic> &topics, const QString &anchor)
{
    for (qsizetype i = 0; i < topics.size(); ++i) {
        if (topics[i].anchor == anchor || topics[i].innerAnchors.contains(anchor)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QStringList guideLinks(const QString &markdown)
{
    static const QRegularExpression link(uR"(\]\(#([^)]+)\))"_s);
    QStringList links;
    for (auto it = link.globalMatch(markdown); it.hasNext();) {
        links.append(it.next().captured(1));
    }
    return links;
}

QStringList guideSearchTerms(const QString &query)
{
    return query.toLower().split(u' ', Qt::SkipEmptyParts);
}

QList<GuideHit> searchGuide(const QList<GuideTopic> &topics, const QString &query, int limit)
{
    const QStringList terms = guideSearchTerms(query);
    if (terms.isEmpty()) {
        return {};
    }
    const QString phrase = terms.join(u' ');

    QList<GuideHit> hits;
    for (qsizetype i = 0; i < topics.size(); ++i) {
        const auto &topic = topics[i];
        if (topic.level == 1) {
            continue; // the home page is only the contents
        }
        const QString title = topic.title.toLower();
        const QString body = topic.plain.toLower();
        int score = 0;
        bool all = true;
        for (const auto &term : terms) {
            const int inTitle = fit(title, term, 60, 45, 15);
            const int inKeywords = fit(topic.keywords, term, 50, 30, 10);
            const int inBody = countIn(body, term);
            if (inTitle == 0 && inKeywords == 0 && inBody == 0) {
                all = false;
                break;
            }
            score += inTitle + (title.startsWith(term) ? 15 : 0) + inKeywords + (inBody * 2) +
                     fit(body, term, 4, 2, 0);
        }
        if (!all) {
            continue;
        }
        if (terms.size() > 1) {
            score += (title.contains(phrase) ? 80 : 0) + (topic.keywords.contains(phrase) ? 60 : 0) +
                     (body.contains(phrase) ? 30 : 0);
        }
        if (topic.level == 2) {
            score /= 2; // the topics say more than their group's introduction
        }
        hits.append({.topic = static_cast<int>(i), .score = score, .snippet = snippet(topic.plain, terms)});
    }
    std::ranges::stable_sort(hits, std::ranges::greater{}, &GuideHit::score);
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    return hits;
}

} // namespace pp

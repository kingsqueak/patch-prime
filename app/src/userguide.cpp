// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "userguide.h"

#include <QTextCharFormat>
#include <QVariantMap>

using namespace Qt::StringLiterals;

UserGuide::UserGuide(const QString &markdown, QObject *parent)
    : QObject(parent), m_topics(pp::parseGuide(markdown))
{
    for (const auto &topic : m_topics) {
        m_topicList.append(QVariantMap{{u"title"_s, topic.title},
                                       {u"anchor"_s, topic.anchor},
                                       {u"group"_s, topic.group},
                                       {u"level"_s, topic.level},
                                       {u"markdown"_s, topic.markdown}});
    }
}

QVariantList UserGuide::search(const QString &query) const
{
    QVariantList results;
    for (const auto &hit : pp::searchGuide(m_topics, query)) {
        const auto &topic = m_topics[hit.topic];
        results.append(QVariantMap{{u"topic"_s, hit.topic},
                                   {u"title"_s, topic.title},
                                   {u"group"_s, topic.level == 3 ? topic.group : QString()},
                                   {u"snippet"_s, hit.snippet}});
    }
    return results;
}

QStringList UserGuide::searchTerms(const QString &query)
{
    return pp::guideSearchTerms(query);
}

int UserGuide::topicForAnchor(const QString &anchor) const
{
    return pp::guideTopicForAnchor(m_topics, anchor);
}

GuideHighlighter::GuideHighlighter(QObject *parent) : QSyntaxHighlighter(parent) {}

void GuideHighlighter::setQuickDocument(QQuickTextDocument *document)
{
    if (m_document == document) {
        return;
    }
    m_document = document;
    setDocument(document != nullptr ? document->textDocument() : nullptr);
    emit quickDocumentChanged();
}

void GuideHighlighter::setTerms(const QStringList &terms)
{
    if (m_terms == terms) {
        return;
    }
    m_terms = terms;
    rehighlight();
    emit termsChanged();
}

void GuideHighlighter::setColor(const QColor &color)
{
    if (m_color != color) {
        m_color = color;
        rehighlight();
        emit colorChanged();
    }
}

void GuideHighlighter::setTextColor(const QColor &color)
{
    if (m_textColor != color) {
        m_textColor = color;
        rehighlight();
        emit colorChanged();
    }
}

void GuideHighlighter::highlightBlock(const QString &text)
{
    QTextCharFormat mark;
    mark.setBackground(m_color);
    mark.setForeground(m_textColor);
    for (const auto &term : m_terms) {
        for (qsizetype at = text.indexOf(term, 0, Qt::CaseInsensitive); at >= 0;
             at = text.indexOf(term, at + term.size(), Qt::CaseInsensitive)) {
            // Whole words and their starts: "to" shouldn't light up every "button".
            if (at == 0 || !text.at(at - 1).isLetterOrNumber()) {
                setFormat(static_cast<int>(at), static_cast<int>(term.size()), mark);
            }
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "guideindex.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QVariantList>

// The user guide (docs/HELP.md, built in) for its window: the topics and search.
class UserGuide : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList topics READ topics CONSTANT)

public:
    explicit UserGuide(const QString &markdown, QObject *parent = nullptr);

    // [{title, anchor, group, level, markdown}]
    [[nodiscard]] QVariantList topics() const { return m_topicList; }
    // [{topic, title, group, snippet}], best first.
    Q_INVOKABLE [[nodiscard]] QVariantList search(const QString &query) const;
    Q_INVOKABLE [[nodiscard]] static QStringList searchTerms(const QString &query);
    Q_INVOKABLE [[nodiscard]] int topicForAnchor(const QString &anchor) const;

private:
    QList<pp::GuideTopic> m_topics;
    QVariantList m_topicList;
};

// Marks the words searched for in the page on show.
class GuideHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ quickDocument WRITE setQuickDocument NOTIFY quickDocumentChanged)
    Q_PROPERTY(QStringList terms READ terms WRITE setTerms NOTIFY termsChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor NOTIFY colorChanged)

public:
    explicit GuideHighlighter(QObject *parent = nullptr);

    [[nodiscard]] QQuickTextDocument *quickDocument() const { return m_document; }
    void setQuickDocument(QQuickTextDocument *document);
    [[nodiscard]] QStringList terms() const { return m_terms; }
    void setTerms(const QStringList &terms);
    [[nodiscard]] QColor color() const { return m_color; }
    void setColor(const QColor &color);
    [[nodiscard]] QColor textColor() const { return m_textColor; }
    void setTextColor(const QColor &color);

signals:
    void quickDocumentChanged();
    void termsChanged();
    void colorChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    QPointer<QQuickTextDocument> m_document;
    QStringList m_terms;
    QColor m_color;
    QColor m_textColor;
};

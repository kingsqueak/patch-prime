// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The user guide (docs/HELP.md, built in) in a window of its own, so it can
// stay open beside the mixer: an index by subject on the left, one topic at a
// time on the right, and a search box over everything. Links to a section
// (#meters) go there; other links open in the browser.
QQC2.ApplicationWindow {
    id: guide

    readonly property var topics: Guide.topics
    property int current: 0
    // The topics read in this window, for Back and Forward.
    property var history: [0]
    property int historyAt: 0

    readonly property var currentTopic: topics.length > 0 ? topics[current] : null
    readonly property bool searching: search.text.trim().length > 0
    readonly property var results: searching ? Guide.search(search.text) : []
    readonly property var terms: searching ? Guide.searchTerms(search.text) : []

    // The index shows the groups, and the topics of the group being read.
    readonly property var indexRows: {
        const group = currentTopic ? currentTopic.group : "";
        const rows = [];
        topics.forEach((topic, i) => {
            if (topic.level < 3 || topic.group === group)
                rows.push({ index: i, title: topic.title, level: topic.level });
        });
        return rows;
    }
    onIndexRowsChanged: Qt.callLater(() => {
        const row = indexRows.findIndex(r => r.index === current);
        if (row >= 0)
            indexList.positionViewAtIndex(row, ListView.Center);
    })

    // The best match opens as you type, without filling Back with every letter.
    onResultsChanged: {
        resultList.currentIndex = 0;
        if (results.length > 0)
            openTopic(results[0].topic, false);
        else
            Qt.callLater(showStart);
    }

    function openTopic(i, remember) {
        if (i < 0 || i >= topics.length)
            return;
        if (remember !== false && i !== current) {
            history = history.slice(0, historyAt + 1).concat([i]);
            historyAt = history.length - 1;
        }
        current = i;
        Qt.callLater(showStart);
    }
    function goBack() {
        if (historyAt > 0) {
            historyAt -= 1;
            openTopic(history[historyAt], false);
        }
    }
    function goForward() {
        if (historyAt < history.length - 1) {
            historyAt += 1;
            openTopic(history[historyAt], false);
        }
    }
    // A link: a topic, or a heading inside one.
    function jumpTo(anchor) {
        openTopic(Guide.topicForAnchor(anchor));
    }

    // The first word searched for, or the top of the page.
    function showStart() {
        const shown = page.getText(0, page.length).toLowerCase();
        for (const term of terms) {
            const at = shown.indexOf(term);
            if (at >= 0) {
                scrollTo(at);
                return;
            }
        }
        scrollTo(0);
    }
    function scrollTo(position) {
        // The text area keeps its cursor in view: move it there first, or
        // it scrolls straight back.
        page.cursorPosition = position;
        const y = position > 0 ? page.positionToRectangle(position).y - Kirigami.Units.gridUnit * 3 : 0;
        scroll.contentItem.contentY = Math.max(0, Math.min(y, scroll.contentItem.contentHeight - scroll.height));
    }

    function show(anchor, query) {
        if (!visible) {
            width = Math.min(Screen.desktopAvailableWidth * 0.9, Kirigami.Units.gridUnit * 58);
            height = Math.min(Screen.desktopAvailableHeight * 0.85, Kirigami.Units.gridUnit * 46);
        }
        visible = true;
        raise();
        requestActivate();
        search.text = query || "";
        if (anchor)
            jumpTo(anchor);
        search.forceActiveFocus();
    }

    function openResult() {
        const hit = results[resultList.currentIndex];
        if (hit)
            openTopic(hit.topic);
    }

    title: qsTr("User Guide")
    minimumWidth: Kirigami.Units.gridUnit * 34
    minimumHeight: Kirigami.Units.gridUnit * 18
    color: Kirigami.Theme.backgroundColor

    Shortcut {
        sequences: [StandardKey.Close]
        onActivated: guide.close()
    }
    Shortcut {
        sequences: ["Esc"]
        onActivated: {
            if (guide.searching)
                search.clear();
            else
                guide.close();
        }
    }
    Shortcut {
        sequences: [StandardKey.Back]
        onActivated: guide.goBack()
    }
    Shortcut {
        sequences: [StandardKey.Forward]
        onActivated: guide.goForward()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Search box, and the index or the results below it.
        ColumnLayout {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 19
            Layout.minimumWidth: Kirigami.Units.gridUnit * 14
            Layout.fillHeight: true
            spacing: 0

            Kirigami.SearchField {
                id: search

                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.smallSpacing
                // Ctrl+F comes with the field.
                placeholderText: qsTr("Search the guide…")
                Accessible.name: qsTr("Search the user guide")
                autoAccept: true
                delaySearch: false
                Keys.onDownPressed: {
                    resultList.incrementCurrentIndex();
                    guide.openResult();
                }
                Keys.onUpPressed: {
                    resultList.decrementCurrentIndex();
                    guide.openResult();
                }
                Keys.onReturnPressed: guide.openResult()
                Keys.onEnterPressed: guide.openResult()
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            QQC2.ScrollView {
                visible: !guide.searching
                Layout.fillWidth: true
                Layout.fillHeight: true
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

                ListView {
                    id: indexList

                    clip: true
                    model: guide.indexRows
                    activeFocusOnTab: true
                    Accessible.name: qsTr("User guide index")

                    delegate: QQC2.ItemDelegate {
                        required property var modelData

                        width: ListView.view.width
                        highlighted: modelData.index === guide.current
                        leftPadding: modelData.level === 3 ? Kirigami.Units.gridUnit * 1.5
                                                           : Kirigami.Units.largeSpacing
                        text: modelData.level === 1 ? qsTr("Contents") : modelData.title
                        font.bold: modelData.level < 3
                        onClicked: guide.openTopic(modelData.index)

                        contentItem: QQC2.Label {
                            text: parent.text
                            font: parent.font
                            wrapMode: Text.Wrap
                            color: parent.highlighted ? Kirigami.Theme.highlightedTextColor
                                                      : Kirigami.Theme.textColor
                        }
                    }
                }
            }

            QQC2.ScrollView {
                visible: guide.searching
                Layout.fillWidth: true
                Layout.fillHeight: true
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

                ListView {
                    id: resultList

                    clip: true
                    model: guide.results
                    Accessible.name: qsTr("Search results")

                    delegate: QQC2.ItemDelegate {
                        id: result

                        required property var modelData
                        required property int index

                        width: ListView.view.width
                        highlighted: ListView.isCurrentItem
                        onClicked: {
                            resultList.currentIndex = index;
                            guide.openResult();
                        }
                        Accessible.name: modelData.title

                        contentItem: ColumnLayout {
                            spacing: 0

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: result.modelData.title
                                font.bold: true
                                wrapMode: Text.Wrap
                                color: result.highlighted ? Kirigami.Theme.highlightedTextColor
                                                          : Kirigami.Theme.textColor
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: result.modelData.group
                                font: Kirigami.Theme.smallFont
                                opacity: 0.7
                                elide: Text.ElideRight
                                color: result.highlighted ? Kirigami.Theme.highlightedTextColor
                                                          : Kirigami.Theme.textColor
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: result.modelData.snippet
                                textFormat: Text.StyledText
                                font: Kirigami.Theme.smallFont
                                wrapMode: Text.Wrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
                                color: result.highlighted ? Kirigami.Theme.highlightedTextColor
                                                          : Kirigami.Theme.textColor
                            }
                        }
                    }

                    Kirigami.PlaceholderMessage {
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.gridUnit * 2
                        visible: guide.searching && guide.results.length === 0
                        text: qsTr("No matches")
                        explanation: qsTr("Try a shorter or a more common word.")
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        // The page.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.smallSpacing
                spacing: Kirigami.Units.smallSpacing

                QQC2.ToolButton {
                    icon.name: "go-previous-view"
                    enabled: guide.historyAt > 0
                    display: QQC2.AbstractButton.IconOnly
                    text: qsTr("Back")
                    QQC2.ToolTip.text: qsTr("Back")
                    QQC2.ToolTip.visible: hovered
                    onClicked: guide.goBack()
                }
                QQC2.ToolButton {
                    icon.name: "go-next-view"
                    enabled: guide.historyAt < guide.history.length - 1
                    display: QQC2.AbstractButton.IconOnly
                    text: qsTr("Forward")
                    QQC2.ToolTip.text: qsTr("Forward")
                    QQC2.ToolTip.visible: hovered
                    onClicked: guide.goForward()
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    opacity: 0.7
                    // Where this topic sits; a group's or the home page's own
                    // title is the heading below.
                    text: guide.currentTopic && guide.currentTopic.level === 3 ? guide.currentTopic.group : ""
                }
                QQC2.ToolButton {
                    icon.name: "go-up"
                    enabled: guide.current > 0
                    display: QQC2.AbstractButton.IconOnly
                    text: qsTr("Previous Topic")
                    QQC2.ToolTip.text: qsTr("Previous Topic")
                    QQC2.ToolTip.visible: hovered
                    onClicked: guide.openTopic(guide.current - 1)
                }
                QQC2.ToolButton {
                    icon.name: "go-down"
                    enabled: guide.current < guide.topics.length - 1
                    display: QQC2.AbstractButton.IconOnly
                    text: qsTr("Next Topic")
                    QQC2.ToolTip.text: qsTr("Next Topic")
                    QQC2.ToolTip.visible: hovered
                    onClicked: guide.openTopic(guide.current + 1)
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            QQC2.ScrollView {
                id: scroll

                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth

                QQC2.TextArea {
                    id: page

                    width: scroll.availableWidth
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    textFormat: TextEdit.MarkdownText
                    text: guide.currentTopic ? guide.currentTopic.markdown
                                             : qsTr("The user guide isn't available in this build.")
                    leftPadding: Kirigami.Units.gridUnit
                    rightPadding: Kirigami.Units.gridUnit
                    topPadding: Kirigami.Units.largeSpacing
                    bottomPadding: Kirigami.Units.gridUnit
                    background: Rectangle {
                        color: Kirigami.Theme.backgroundColor
                    }
                    onLinkActivated: link => {
                        if (link.startsWith("#"))
                            guide.jumpTo(link.slice(1));
                        else
                            Qt.openUrlExternally(link);
                    }

                    GuideHighlighter {
                        document: page.textDocument
                        terms: guide.terms
                        color: Kirigami.Theme.highlightColor
                        textColor: Kirigami.Theme.highlightedTextColor
                    }

                    HoverHandler {
                        enabled: page.hoveredLink.length > 0
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }
    }
}

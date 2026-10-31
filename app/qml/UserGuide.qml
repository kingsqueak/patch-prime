// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Window
import org.kde.kirigami as Kirigami

// The user guide (docs/HELP.md, built into the app) in a window of its own,
// so it can stay open beside the mixer. Links to a section (#meters) jump
// there; other links open in the browser.
QQC2.ApplicationWindow {
    id: guide

    property string markdown

    // Each heading gets an invisible marker (zero-width characters, unique
    // per heading), so its place in the rendered text can be found again:
    // the table of contents repeats the headings' words.
    readonly property var headings: {
        const found = [];
        for (const line of markdown.split("\n")) {
            const m = /^#{1,6}\s+(.*)$/.exec(line);
            if (m)
                found.push(m[1].trim());
        }
        return found;
    }
    readonly property string marked: {
        let i = 0;
        return markdown.split("\n").map(line => /^#{1,6}\s+/.test(line)
                                                ? line.replace(/\s+$/, "") + guide.marker(i++) : line).join("\n");
    }

    function marker(i) {
        return "\u200C" + "\u200B".repeat(i + 1) + "\u200D";
    }
    // GitHub's anchor for a heading: lower case, punctuation dropped, spaces to dashes.
    function slug(heading) {
        return heading.toLowerCase().replace(/[^\p{L}\p{N}\s-]/gu, "").replace(/\s/g, "-");
    }
    function show() {
        if (!visible) {
            width = Kirigami.Units.gridUnit * 44;
            height = Math.min(Screen.desktopAvailableHeight * 0.85, Kirigami.Units.gridUnit * 46);
        }
        visible = true;
        raise();
        requestActivate();
    }
    function jumpTo(anchor) {
        const i = headings.findIndex(h => slug(h) === anchor);
        if (i < 0)
            return;
        const at = text.getText(0, text.length).indexOf(marker(i));
        if (at < 0)
            return;
        // The text area keeps its cursor in view: move it there first, or
        // it scrolls straight back.
        text.cursorPosition = at;
        scroll.contentItem.contentY = Math.max(0, Math.min(text.positionToRectangle(at).y
                                                           - Kirigami.Units.largeSpacing,
                                                           scroll.contentItem.contentHeight - scroll.height));
    }

    title: qsTr("User Guide — Patch Prime")
    minimumWidth: Kirigami.Units.gridUnit * 24
    minimumHeight: Kirigami.Units.gridUnit * 16
    color: Kirigami.Theme.backgroundColor

    QQC2.ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth

        QQC2.TextArea {
            id: text

            width: scroll.availableWidth
            readOnly: true
            selectByMouse: true
            wrapMode: Text.Wrap
            textFormat: TextEdit.MarkdownText
            text: guide.marked.length > 0 ? guide.marked
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

            HoverHandler {
                enabled: text.hoveredLink.length > 0
                cursorShape: Qt.PointingHandCursor
            }
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The mixer's app strips: every running app that plays sound, except the
// ones hidden from the mixer (shown again from the Apps page). Titled like
// StripGroup; clicking the title collapses them into one column of compact
// rows (AppCompactRow), for when many apps play at once. Remembered.
ColumnLayout {
    id: group

    // The apps shown, by key; only reassigned when that list changes, so
    // the strips aren't rebuilt on every level change.
    property var keys: []
    readonly property int count: keys.length
    property bool collapsed: false

    function refresh() {
        const wanted = Devices.apps
            .filter(a => a.direction === "playback" && a.streams > 0 && !a.hidden)
            .map(a => a.key);
        if (JSON.stringify(wanted) !== JSON.stringify(keys))
            keys = wanted;
    }

    spacing: Style.gap
    Component.onCompleted: refresh()

    Settings {
        category: "Mixer"
        property alias appsCollapsed: group.collapsed
    }

    Connections {
        target: Devices
        function onAppsChanged() { group.refresh(); }
    }

    // The title, a button: collapses the strips into one column and back.
    RowLayout {
        spacing: Style.gap

        Rectangle {
            implicitWidth: Kirigami.Units.gridUnit
            implicitHeight: Style.stageBarHeight
            radius: height / 2
            color: Style.appColor
        }

        Kirigami.Heading {
            level: 5
            text: (group.collapsed ? qsTr("Apps (%1)").arg(group.count) : qsTr("Apps")).toUpperCase()
            font.letterSpacing: 1
            color: titleHover.hovered ? Kirigami.Theme.textColor : Style.dimText
        }

        Kirigami.Icon {
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            source: group.collapsed ? "arrow-right" : "arrow-left"
            color: titleHover.hovered ? Kirigami.Theme.textColor : Style.dimText
            isMask: true
        }

        HoverHandler {
            id: titleHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: group.collapsed = !group.collapsed
        }

        QQC2.ToolTip.text: group.collapsed ? qsTr("Show each app as a strip") : qsTr("Fold the apps into one column")
        QQC2.ToolTip.visible: titleHover.hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    // Expanded: a strip per app.
    RowLayout {
        Layout.fillHeight: true
        visible: !group.collapsed
        spacing: Style.stripGap

        Repeater {
            model: group.collapsed ? [] : group.keys

            delegate: AppStrip {
                required property string modelData
                Layout.fillHeight: true
                appKey: modelData
            }
        }
    }

    // Collapsed: one strip-wide column of compact rows, scrolling when full.
    QQC2.Control {
        Layout.fillHeight: true
        Layout.preferredWidth: Style.stripWidth
        visible: group.collapsed
        padding: Style.stripPadding
        topPadding: Style.stripPadding + Style.stageBarHeight

        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false

        background: Rectangle {
            color: Kirigami.Theme.backgroundColor
            border.color: Style.stripBorder
            border.width: 1
            radius: Style.stripRadius

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                height: Style.stageBarHeight
                radius: Style.stripRadius
                color: Style.appColor
            }
        }

        contentItem: QQC2.ScrollView {
            id: scroller
            contentWidth: availableWidth
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ColumnLayout {
                width: scroller.availableWidth
                spacing: Kirigami.Units.largeSpacing

                Repeater {
                    model: group.collapsed ? group.keys : []

                    delegate: AppCompactRow {
                        required property string modelData
                        Layout.fillWidth: true
                        appKey: modelData
                    }
                }
            }
        }
    }
}

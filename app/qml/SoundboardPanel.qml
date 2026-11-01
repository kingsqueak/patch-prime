// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The soundboard in the mixer: its pads in a strip-wide column, at hand
// while mixing (the Soundboard page adds and edits them).
ColumnLayout {
    id: panel

    signal openSoundboard()

    spacing: Style.gap

    RowLayout {
        spacing: Style.gap

        Rectangle {
            implicitWidth: Kirigami.Units.gridUnit
            implicitHeight: Style.stageBarHeight
            radius: height / 2
            color: Kirigami.Theme.highlightColor
        }

        Kirigami.Heading {
            level: 5
            text: qsTr("Soundboard").toUpperCase()
            font.letterSpacing: 1
            color: titleHover.hovered ? Kirigami.Theme.textColor : Style.dimText
            HoverHandler {
                id: titleHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: panel.openSoundboard()
            }
        }
    }

    QQC2.Control {
        Layout.fillHeight: true
        Layout.preferredWidth: Style.stripWidth
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
                color: Kirigami.Theme.highlightColor
            }
        }

        contentItem: ColumnLayout {
            spacing: Style.gap

            QQC2.ScrollView {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

                ColumnLayout {
                    width: scroller.availableWidth
                    spacing: Style.gap

                    Repeater {
                        model: Devices.pads

                        delegate: PadButton {
                            Layout.fillWidth: true
                            display: QQC2.AbstractButton.TextBesideIcon
                        }
                    }
                }
            }

            QQC2.Button {
                Layout.fillWidth: true
                text: qsTr("Stop All")
                icon.name: "media-playback-stop"
                enabled: Devices.pads.some(p => p.playing)
                onClicked: Devices.stopAllPads()
            }
        }
    }
}

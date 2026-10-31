// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The mixer's Recordings panel, beside the outputs and three strips wide:
// the player and its settings at the top, the files below (scrolling).
// The Recordings page has the same, roomier.
ColumnLayout {
    id: panel

    readonly property RecordingsState rec: RecordingsState {}

    spacing: Style.gap

    // The title, like a strip group's.
    RowLayout {
        spacing: Style.gap

        Rectangle {
            implicitWidth: Kirigami.Units.gridUnit
            implicitHeight: Style.stageBarHeight
            radius: height / 2
            color: Style.recordOn
        }
        Kirigami.Heading {
            level: 5
            text: qsTr("Recordings").toUpperCase()
            font.letterSpacing: 1
            color: Style.dimText
        }
    }

    QQC2.Control {
        Layout.fillHeight: true
        Layout.preferredWidth: Style.stripWidth * 3 + Style.stripGap * 2
        padding: Style.stripPadding
        topPadding: Style.stripPadding + Style.stageBarHeight

        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false

        background: Rectangle {
            color: Kirigami.Theme.backgroundColor
            border.color: Style.stripBorder
            radius: Style.stripRadius

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                height: Style.stageBarHeight
                radius: Style.stripRadius
                color: Style.recordOn
            }
        }

        contentItem: ColumnLayout {
            spacing: Style.gap

            PlayerControls {
                Layout.fillWidth: true
                rec: panel.rec
            }

            QQC2.ComboBox {
                Layout.fillWidth: true
                font: Kirigami.Theme.smallFont
                model: panel.rec.targets
                textRole: "label"
                valueRole: "key"
                enabled: panel.rec.targets.length > 0
                currentIndex: panel.rec.targets.findIndex(t => t.key === panel.rec.target)
                onActivated: Devices.setPlayerTarget(currentValue)
                Accessible.name: qsTr("Play into")
                QQC2.ToolTip.text: panel.rec.targetInfo.input === true
                    ? qsTr("Play into %1: it hears the recording instead of its device, through its effects, while playing.").arg(panel.rec.targetInfo.name)
                    : qsTr("Where the player plays")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Style.gap

                QQC2.ComboBox {
                    Layout.fillWidth: true
                    font: Kirigami.Theme.smallFont
                    model: panel.rec.formats
                    textRole: "text"
                    valueRole: "value"
                    displayText: qsTr("Record as %1").arg(currentIndex >= 0 ? panel.rec.formats[currentIndex].short : "")
                    currentIndex: Math.max(0, panel.rec.formats.findIndex(m => m.value === Devices.recordingFormat))
                    onActivated: Devices.setRecordingFormat(currentValue)
                    Accessible.name: qsTr("Recording format")
                }
                QQC2.ToolButton {
                    icon.name: "folder-open"
                    text: qsTr("Open Folder")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: Devices.recordingFolder.length > 0
                    onClicked: Qt.openUrlExternally("file://" + Devices.recordingFolder)
                    QQC2.ToolTip.text: qsTr("Open %1").arg(Devices.recordingFolder)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: Devices.recordingError.length > 0
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.negativeTextColor
                text: qsTr("A recording stopped: %1").arg(Devices.recordingError)
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            // The files, scrolling.
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth

                ListView {
                    model: panel.rec.files
                    clip: true
                    delegate: RecordingRow {
                        width: ListView.view.width
                        rec: panel.rec
                        compact: true
                    }

                    QQC2.Label {
                        anchors.fill: parent
                        anchors.margins: Kirigami.Units.smallSpacing
                        visible: panel.rec.files.length === 0
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font: Kirigami.Theme.smallFont
                        opacity: 0.6
                        text: qsTr("No recordings yet. Press Record on an input, a mix bus or Desktop.")
                    }
                }
            }
        }
    }
}

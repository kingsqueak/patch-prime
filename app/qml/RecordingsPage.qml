// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Recordings: the files the record buttons made, the player that plays them
// into a strip, and the recording format. (The mixer has a compact version
// of this: RecordingsPanel.)
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Recordings")

    header: EngineMessage {}

    readonly property RecordingsState rec: RecordingsState {}

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: Devices.recordingError.length > 0
            type: Kirigami.MessageType.Error
            text: qsTr("A recording stopped: %1").arg(Devices.recordingError)
        }

        // --- the player
        Kirigami.AbstractCard {
            Layout.fillWidth: true

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                PlayerControls {
                    Layout.fillWidth: true
                    rec: page.rec
                }
                QQC2.Label {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    text: qsTr("Play into")
                }
                QQC2.ComboBox {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 15
                    model: page.rec.targets
                    textRole: "label"
                    valueRole: "key"
                    enabled: page.rec.targets.length > 0
                    currentIndex: page.rec.targets.findIndex(t => t.key === page.rec.target)
                    onActivated: Devices.setPlayerTarget(currentValue)
                    Accessible.name: qsTr("Play into")
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.rec.targetInfo.input === true
            type: Kirigami.MessageType.Information
            text: page.rec.playing.length > 0
                  ? qsTr("%1 hears this recording instead of its device, through its effects, until playback stops. The device is cut off meanwhile.").arg(page.rec.targetInfo.name)
                  : qsTr("While playing, %1 hears the recording instead of its device, through its effects. Record a raw sample with the input's Record button, then loop it here while you set up the effects.").arg(page.rec.targetInfo.name)
        }

        // --- settings
        RowLayout {
            Layout.fillWidth: true

            QQC2.Label {
                text: qsTr("Record as")
            }
            QQC2.ComboBox {
                model: page.rec.formats
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, page.rec.formats.findIndex(m => m.value === Devices.recordingFormat))
                onActivated: Devices.setRecordingFormat(currentValue)
                Accessible.name: qsTr("Recording format")
            }
            Item { Layout.fillWidth: true }
            QQC2.Button {
                icon.name: "folder-open"
                text: qsTr("Open Folder")
                enabled: Devices.recordingFolder.length > 0
                onClicked: Qt.openUrlExternally("file://" + Devices.recordingFolder)
            }
        }

        // --- the files
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 2
            visible: page.rec.files.length === 0
            icon.name: "media-record"
            text: qsTr("No recordings yet")
            explanation: qsTr("Press Record on an input, a mix bus or Desktop in the mixer. Recordings go to %1.")
                         .arg(Devices.recordingFolder)
        }

        Repeater {
            model: page.rec.files

            delegate: RecordingRow {
                Layout.fillWidth: true
                rec: page.rec
            }
        }
    }
}

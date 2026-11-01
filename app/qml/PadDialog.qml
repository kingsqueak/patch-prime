// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Edits a soundboard pad: its name, level and loop (the file stays).
// Made for one pad and destroyed when closed.
Kirigami.Dialog {
    id: dialog

    required property var pad

    title: qsTr("Edit Pad")
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.NoButton
    customFooterActions: [
        Kirigami.Action {
            text: qsTr("Save")
            icon.name: "document-save"
            onTriggered: {
                Devices.updatePad(dialog.pad.id, label.text, level.value, loop.checked);
                dialog.close();
            }
        },
        Kirigami.Action {
            text: qsTr("Cancel")
            icon.name: "dialog-cancel"
            onTriggered: dialog.close()
        }
    ]
    onClosed: destroy()

    Kirigami.FormLayout {
        QQC2.TextField {
            id: label
            Kirigami.FormData.label: qsTr("Name:")
            Layout.preferredWidth: Kirigami.Units.gridUnit * 16
            text: dialog.pad.label
        }

        RowLayout {
            Kirigami.FormData.label: qsTr("Level:")

            VolumeFader {
                id: level
                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                orientation: Qt.Horizontal
                volumeDb: dialog.pad.gainDb
                accessibleName: qsTr("Level")
                onRequested: db => value = db
            }

            QQC2.Label {
                text: level.value <= Devices.minimumVolumeDb ? qsTr("−∞ dB") : qsTr("%1 dB").arg(level.value.toFixed(1))
                font.family: "monospace"
            }
        }

        QQC2.CheckBox {
            id: loop
            Kirigami.FormData.label: qsTr("Loop:")
            text: qsTr("Play until clicked again")
            checked: dialog.pad.loop
        }

        QQC2.Label {
            Kirigami.FormData.label: qsTr("File:")
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            text: dialog.pad.path
            elide: Text.ElideMiddle
            color: Kirigami.Theme.disabledTextColor
        }
    }
}

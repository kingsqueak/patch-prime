// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The mixer's scene row: pick a scene, then Load, Save… (as a new name or
// over the picked one) or Delete. A scene is the mix: levels, mutes, mono,
// sends, app routes and the default output and input.
RowLayout {
    id: bar

    spacing: Style.gap

    QQC2.Label {
        text: qsTr("Scene:")
    }

    QQC2.ComboBox {
        id: choice
        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
        enabled: Devices.daemonRunning && Devices.scenes.length > 0
        model: Devices.scenes
        displayText: Devices.scenes.length === 0 ? qsTr("No scenes yet") : currentText
        // Show the scene last saved or loaded.
        currentIndex: Math.max(0, Devices.scenes.indexOf(Devices.currentScene))
        Accessible.name: qsTr("Scene")
    }

    QQC2.Button {
        text: qsTr("Load")
        icon.name: "document-open"
        enabled: Devices.daemonRunning && Devices.scenes.length > 0
        onClicked: Devices.loadScene(choice.currentText)
        QQC2.ToolTip.text: qsTr("Set the mix to this scene: levels, mutes, sends, app routes and defaults")
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    QQC2.Button {
        text: qsTr("Save…")
        icon.name: "document-save"
        enabled: Devices.daemonRunning
        onClicked: saveDialog.open()
    }

    QQC2.Button {
        text: qsTr("Delete")
        icon.name: "edit-delete-remove"
        display: QQC2.AbstractButton.IconOnly
        enabled: Devices.daemonRunning && Devices.scenes.length > 0
        onClicked: deleteDialog.open()
        QQC2.ToolTip.text: qsTr("Delete this scene")
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    Item {
        Layout.fillWidth: true
    }

    QQC2.Label {
        visible: Devices.currentScene.length > 0
        text: qsTr("Current: %1").arg(Devices.currentScene)
        color: Style.dimText
        font: Kirigami.Theme.smallFont
    }

    Kirigami.PromptDialog {
        id: saveDialog

        title: qsTr("Save Scene")
        standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel
        onOpened: {
            nameField.text = Devices.currentScene;
            nameField.forceActiveFocus();
            nameField.selectAll();
        }
        onAccepted: {
            if (nameField.text.trim().length > 0)
                Devices.saveScene(nameField.text);
        }

        ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. Streaming")
                onAccepted: saveDialog.accept()
            }

            QQC2.Label {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Style.dimText
                text: Devices.scenes.indexOf(nameField.text.trim()) >= 0
                      ? qsTr("Replaces the scene with this name.")
                      : qsTr("Saves the current levels, mutes, sends, app routes and defaults.")
            }
        }
    }

    Kirigami.PromptDialog {
        id: deleteDialog

        title: qsTr("Delete Scene")
        subtitle: qsTr("Delete the scene \"%1\"? The current mix stays as it is.").arg(choice.currentText)
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onAccepted: Devices.deleteScene(choice.currentText)
    }
}

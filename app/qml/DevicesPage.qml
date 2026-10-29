// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

Kirigami.ScrollablePage {
    id: page

    title: qsTr("Devices")

    header: EngineMessage {}

    actions: Kirigami.Action {
        text: qsTr("Add Virtual Device…")
        icon.name: "list-add"
        enabled: Devices.daemonRunning
        onTriggered: addDialog.open()
    }

    Kirigami.PromptDialog {
        id: addDialog

        title: qsTr("Add Virtual Device")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onOpened: {
            nameField.text = "";
            nameField.forceActiveFocus();
        }
        onAccepted: {
            if (nameField.text.trim().length > 0)
                Devices.createVirtual(nameField.text, inputButton.checked ? "input"
                                                    : sinkButton.checked ? "sink" : "source");
        }

        ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: inputButton.checked ? qsTr("e.g. Desktop")
                               : sinkButton.checked ? qsTr("e.g. Stream Mix") : qsTr("e.g. Chat Mic")
                onAccepted: addDialog.accept()
            }

            QQC2.RadioButton {
                id: inputButton
                checked: true
                text: qsTr("Input: apps play into it, and you route it to outputs (desktop sound on several headphones)")
            }

            QQC2.RadioButton {
                id: sinkButton
                text: qsTr("Mix bus: inputs and apps mix into it, it goes to outputs, and apps can record it (a stream mix for OBS)")
            }

            QQC2.RadioButton {
                text: qsTr("Microphone: apps record from it (a mic for Discord)")
            }
        }
    }

    ListView {
        id: list

        model: Devices

        // Settings for the whole mixer, above the device list.
        header: ColumnLayout {
            width: ListView.view.width
            spacing: 0

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                text: qsTr("Audio clock")
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                QQC2.ComboBox {
                    id: clockChoice
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    enabled: Devices.daemonRunning
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { value: "internal", text: qsTr("Patch Prime") },
                        { value: "hardware", text: qsTr("Hardware device") },
                    ]
                    currentIndex: Devices.clock === "hardware" ? 1 : 0
                    onActivated: Devices.setClock(currentValue)
                    Accessible.name: qsTr("Audio clock")
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: clockChoice.currentIndex === 1
                        ? qsTr("PipeWire uses an interface's own clock: the lowest latency, but unplugging that interface briefly stops all audio, and some apps pause.")
                        : qsTr("Patch Prime keeps time itself, so unplugging any interface doesn't interrupt the others. Interfaces follow it with PipeWire's rate matching.")
                }
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: list.count === 0
            icon.name: "audio-card"
            text: !Devices.daemonRunning ? qsTr("Waiting for the engine")
                : Devices.connected ? qsTr("No audio devices") : qsTr("Waiting for PipeWire")
        }

        // Section headers are drawn by the delegate, not ListView.section: with
        // Qt 6.10, inserting a row above a section boundary left a delegate
        // showing another row's data.
        delegate: ColumnLayout {
            id: row

            // Role values come through `model`, which follows the row when rows
            // are inserted above it, rather than one required property per role.
            required property var model
            readonly property string key: model.key
            readonly property string customLabel: model.customLabel
            readonly property string description: model.description
            readonly property string nodeName: model.nodeName
            readonly property string direction: model.direction
            readonly property bool isVirtual: model.isVirtual
            readonly property bool online: model.online
            readonly property bool isEnabled: model.isEnabled
            readonly property int channels: model.channels
            readonly property string positions: model.positions
            readonly property bool isDefault: model.isDefault
            readonly property bool saved: model.saved
            readonly property bool sectionStart: model.sectionStart
            readonly property string kind: model.kind
            readonly property bool ownVirtual: kind !== "hardware"

            width: ListView.view.width
            spacing: 0

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                visible: row.sectionStart
                text: row.direction === "input" ? qsTr("Inputs")
                                                : qsTr("Outputs and virtual devices")
            }

            QQC2.ItemDelegate {
                Layout.fillWidth: true
                hoverEnabled: false
                down: false

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    QQC2.CheckBox {
                        checked: row.isEnabled
                        onToggled: Devices.setEnabled(row.key, checked)
                        QQC2.ToolTip.text: qsTr("Use this device in Patch Prime")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }

                    Kirigami.Icon {
                        source: row.kind === "virtual-input" ? "audio-volume-high"
                              : row.direction === "input" ? "audio-input-microphone"
                              : row.isVirtual ? "audio-volume-high" : "audio-speakers"
                        implicitWidth: Kirigami.Units.iconSizes.medium
                        implicitHeight: Kirigami.Units.iconSizes.medium
                        opacity: row.online ? 1 : 0.4
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        QQC2.TextField {
                            Layout.fillWidth: true
                            text: row.customLabel
                            placeholderText: row.description
                            onEditingFinished: Devices.setLabel(row.key, text)
                            Accessible.name: qsTr("Label for %1").arg(row.description)
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                            text: {
                                const parts = [];
                                if (row.customLabel.length > 0)
                                    parts.push(row.description);
                                parts.push(row.kind === "virtual-input" ? qsTr("Patch Prime virtual input")
                                           : row.kind === "virtual-sink" ? qsTr("Patch Prime mix bus")
                                           : row.kind === "virtual-source" ? qsTr("Patch Prime virtual microphone")
                                           : row.isVirtual ? qsTr("virtual") : qsTr("hardware"));
                                if (row.channels > 0)
                                    parts.push(row.positions.length > 0
                                               ? qsTr("%1 ch (%2)").arg(row.channels).arg(row.positions)
                                               : qsTr("%1 ch").arg(row.channels));
                                parts.push(row.nodeName);
                                return parts.join(" · ");
                            }
                        }
                    }

                    QQC2.Label {
                        visible: row.isDefault
                        text: qsTr("Default")
                        color: Kirigami.Theme.positiveTextColor
                        QQC2.ToolTip.text: row.direction === "input"
                                           ? qsTr("The system's default input")
                                           : qsTr("The system's default output")
                        QQC2.ToolTip.visible: defaultHover.hovered
                        HoverHandler { id: defaultHover }
                    }

                    QQC2.Label {
                        visible: !row.online
                        text: qsTr("Offline")
                        color: Kirigami.Theme.neutralTextColor
                    }

                    QQC2.ToolButton {
                        visible: row.ownVirtual || (!row.online && row.saved)
                        icon.name: "edit-delete-remove"
                        text: row.ownVirtual ? qsTr("Delete") : qsTr("Forget")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: Devices.forget(row.key)
                        QQC2.ToolTip.text: row.ownVirtual ? qsTr("Delete this virtual device")
                                                          : qsTr("Forget this device's settings")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }
            }
        }
    }
}

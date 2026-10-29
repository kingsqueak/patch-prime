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

    header: Kirigami.InlineMessage {
        visible: !Devices.connected
        position: Kirigami.InlineMessage.Header
        type: Kirigami.MessageType.Warning
        text: qsTr("Not connected to PipeWire. Retrying…")
    }

    ListView {
        id: list

        model: Devices

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: list.count === 0
            icon.name: "audio-card"
            text: Devices.connected ? qsTr("No audio devices") : qsTr("Waiting for PipeWire")
        }

        // Section headers are drawn by the delegate, not ListView.section: with
        // Qt 6.10, inserting a row above a section boundary left a delegate
        // showing another row's data.
        delegate: ColumnLayout {
            id: row

            // Role values come through `model`, which follows the row when rows
            // are inserted above it, rather than one required property per role.
            required property int index
            required property var model
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

            width: ListView.view.width
            spacing: 0

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                visible: row.sectionStart
                text: row.direction === "input" ? qsTr("Inputs") : qsTr("Outputs")
            }

            QQC2.ItemDelegate {
                Layout.fillWidth: true
                hoverEnabled: false
                down: false

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    QQC2.CheckBox {
                        checked: row.isEnabled
                        onToggled: Devices.setEnabled(row.index, checked)
                        QQC2.ToolTip.text: qsTr("Use this device in Patch Prime")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }

                    Kirigami.Icon {
                        source: row.direction === "input" ? "audio-input-microphone"
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
                            onEditingFinished: Devices.setLabel(row.index, text)
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
                                parts.push(row.isVirtual ? qsTr("virtual") : qsTr("hardware"));
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
                        visible: !row.online && row.saved
                        icon.name: "edit-delete-remove"
                        text: qsTr("Forget")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: Devices.forget(row.index)
                        QQC2.ToolTip.text: qsTr("Forget this device's settings")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }
            }
        }
    }
}

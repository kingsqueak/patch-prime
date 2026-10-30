// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// What feeds what, console style. Rows are the strips that send (inputs, then
// mix buses), columns the ones that receive (mix buses, then outputs). Sends
// only go to a later stage, so a mix bus row has ticks only under outputs.
// A ticked cell has a slider for that send's level (0 dB by default), so one
// input can go to two places at different levels.
Kirigami.Page {
    id: page

    title: qsTr("Routing")
    header: EngineMessage {}
    padding: 0

    readonly property real nameWidth: Kirigami.Units.gridUnit * 10
    readonly property real cellWidth: Kirigami.Units.gridUnit * 6
    readonly property real rowHeight: Kirigami.Units.gridUnit * 3.5

    // Stage order: a send goes from a lower rank to a higher one.
    function rank(stage) {
        return stage === "input" ? 0 : stage === "bus" ? 1 : 2;
    }

    StripFilter {
        id: inputs // the senders
        sourceModel: Devices
        stages: ["input", "bus"]
    }

    StripFilter {
        id: outputs // the destinations
        sourceModel: Devices
        stages: ["bus", "output"]
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: inputs.count === 0 || outputs.count === 0
        icon.name: "network-connect"
        text: qsTr("Nothing to route yet")
        explanation: qsTr("Enable at least one input and one output, or add a mix bus, on the Devices page.")
    }

    // Scrolls both ways: a grid with many devices can be wider than the window.
    QQC2.ScrollView {
        anchors.fill: parent
        visible: inputs.count > 0 && outputs.count > 0
        contentWidth: grid.implicitWidth + Kirigami.Units.largeSpacing * 2
        contentHeight: grid.implicitHeight + Kirigami.Units.largeSpacing * 2

        ColumnLayout {
            id: grid

            x: Kirigami.Units.largeSpacing
            y: Kirigami.Units.largeSpacing
            spacing: 0

            // Column headings: the destinations.
            RowLayout {
                spacing: 0

                Item {
                    Layout.preferredWidth: page.nameWidth
                }

                Repeater {
                    model: outputs

                    // The destination's name over a bar in its stage's color.
                    delegate: ColumnLayout {
                        required property var model
                        Layout.preferredWidth: page.cellWidth
                        Layout.maximumWidth: page.cellWidth
                        Layout.alignment: Qt.AlignBottom
                        spacing: 2
                        opacity: model.online ? 1 : 0.5

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: parent.model.label
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: parent.model.stage === "bus" ? qsTr("Mix bus") : qsTr("Output")
                            font: Kirigami.Theme.smallFont
                            color: Style.dimText
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: page.cellWidth * 0.6
                            Layout.preferredHeight: Style.stageBarHeight
                            radius: height / 2
                            color: Style.stageColor(parent.model.stage)
                        }
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
            }

            // One row per sender.
            Repeater {
                model: inputs

                delegate: RowLayout {
                    id: inputRow

                    required property var model
                    required property int index
                    readonly property string key: model.key
                    readonly property var sends: model.sends
                    readonly property var sendLevels: model.sendLevels

                    spacing: 0

                    // The sender: a bar in its stage's color, its name and kind.
                    RowLayout {
                        Layout.preferredWidth: page.nameWidth
                        Layout.maximumWidth: page.nameWidth
                        Layout.preferredHeight: page.rowHeight
                        spacing: Kirigami.Units.smallSpacing
                        opacity: inputRow.model.online ? 1 : 0.5

                        Rectangle {
                            Layout.preferredWidth: Style.stageBarHeight
                            Layout.fillHeight: true
                            Layout.topMargin: Kirigami.Units.smallSpacing
                            Layout.bottomMargin: Kirigami.Units.smallSpacing
                            radius: width / 2
                            color: Style.stageColor(inputRow.model.stage)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: inputRow.model.label
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: inputRow.model.stage === "bus" ? qsTr("Mix bus") : qsTr("Input")
                                font: Kirigami.Theme.smallFont
                                color: Style.dimText
                            }
                        }
                    }

                    Repeater {
                        model: outputs

                        // A fixed-size cell, so every row lines up under the headings.
                        delegate: Item {
                            id: cell

                            required property var model
                            readonly property bool sent: inputRow.sends.indexOf(model.key) >= 0
                            readonly property real levelDb: (inputRow.sendLevels ?? {})[model.key] ?? 0
                            readonly property bool allowed: page.rank(model.stage) > page.rank(inputRow.model.stage)

                            Layout.preferredWidth: page.cellWidth
                            Layout.minimumWidth: page.cellWidth
                            Layout.maximumWidth: page.cellWidth
                            Layout.preferredHeight: page.rowHeight

                            // Every other row faintly shaded, to follow it across.
                            Rectangle {
                                anchors.fill: parent
                                color: inputRow.index % 2 === 0 ? Style.toggleOff : "transparent"
                                opacity: 0.6
                            }

                            // The send: a lit button (a tick when on, a + when off).
                            QQC2.AbstractButton {
                                id: tick

                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                anchors.topMargin: Kirigami.Units.smallSpacing
                                width: page.cellWidth * 0.45
                                height: Style.controlHeight
                                visible: cell.allowed
                                enabled: Devices.daemonRunning
                                checkable: true
                                hoverEnabled: true
                                onToggled: Devices.setSend(inputRow.key, cell.model.key, checked)
                                Accessible.name: qsTr("Send %1 to %2").arg(inputRow.model.label).arg(cell.model.label)
                                // Stays bound to the daemon's state after a click.
                                Binding on checked {
                                    value: cell.sent
                                }
                                contentItem: Kirigami.Icon {
                                    source: tick.checked ? "checkmark" : "list-add"
                                    implicitWidth: Kirigami.Units.iconSizes.small
                                    implicitHeight: Kirigami.Units.iconSizes.small
                                    color: tick.checked ? Style.toggleOnText : Style.dimText
                                    isMask: true
                                }
                                background: Rectangle {
                                    radius: Style.toggleRadius
                                    color: tick.checked ? Style.toggleOn : tick.hovered ? Style.toggleHover : "transparent"
                                    border.color: tick.checked ? "transparent" : Style.stripBorder
                                    border.width: 1
                                }
                                QQC2.ToolTip.text: tick.checked ? qsTr("%1 goes to %2. Click to stop.").arg(inputRow.model.label).arg(cell.model.label)
                                                                : qsTr("Send %1 to %2").arg(inputRow.model.label).arg(cell.model.label)
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            }

                            // The send's level, once it's on.
                            VolumeFader {
                                id: level

                                anchors.top: tick.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: Kirigami.Units.smallSpacing
                                anchors.rightMargin: Kirigami.Units.smallSpacing
                                orientation: Qt.Horizontal
                                visible: cell.allowed && cell.sent
                                enabled: Devices.daemonRunning
                                volumeDb: cell.levelDb
                                accessibleName: qsTr("Level of %1 into %2").arg(inputRow.model.label).arg(cell.model.label)
                                onRequested: db => Devices.setSendLevel(inputRow.key, cell.model.key, db)
                            }

                            // Click to type the send level.
                            DbField {
                                anchors.top: level.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: Kirigami.Units.smallSpacing
                                anchors.rightMargin: Kirigami.Units.smallSpacing
                                visible: level.visible
                                font: Kirigami.Theme.smallFont
                                color: Kirigami.Theme.disabledTextColor
                                value: level.pressed ? level.value : cell.levelDb
                                accessibleName: qsTr("Level of %1 into %2 in dB").arg(inputRow.model.label).arg(cell.model.label)
                                onRequested: db => Devices.setSendLevel(inputRow.key, cell.model.key, db)
                            }
                        }
                    }
                }
            }
        }
    }
}

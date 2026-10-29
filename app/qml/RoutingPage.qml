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
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Routing")
    header: EngineMessage {}

    readonly property real nameWidth: Kirigami.Units.gridUnit * 10
    readonly property real cellWidth: Kirigami.Units.gridUnit * 6

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

    ColumnLayout {
        spacing: 0

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 4
            visible: inputs.count === 0 || outputs.count === 0
            icon.name: "network-connect"
            text: qsTr("Nothing to route yet")
            explanation: qsTr("Enable at least one input and one output, or add a mix bus, on the Devices page.")
        }

        // Column headings: the destinations.
        RowLayout {
            visible: inputs.count > 0 && outputs.count > 0
            spacing: 0

            Item {
                Layout.preferredWidth: page.nameWidth
            }

            Repeater {
                model: outputs

                delegate: QQC2.Label {
                    required property var model
                    Layout.preferredWidth: page.cellWidth
                    Layout.alignment: Qt.AlignBottom
                    text: model.stage === "bus" ? qsTr("%1\n(mix bus)").arg(model.label) : model.label
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    opacity: model.online ? 1 : 0.5
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            visible: inputs.count > 0 && outputs.count > 0
        }

        // One row per input.
        Repeater {
            model: outputs.count > 0 ? inputs : null

            delegate: RowLayout {
                id: inputRow

                required property var model
                readonly property string key: model.key
                readonly property var sends: model.sends

                spacing: 0

                QQC2.Label {
                    Layout.preferredWidth: page.nameWidth
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    verticalAlignment: Text.AlignVCenter
                    text: inputRow.model.stage === "bus" ? qsTr("%1 (mix bus)").arg(inputRow.model.label)
                                                         : inputRow.model.label
                    elide: Text.ElideRight
                    opacity: inputRow.model.online ? 1 : 0.5
                }

                Repeater {
                    model: outputs

                    delegate: Item {
                        id: cell

                        required property var model
                        readonly property bool sent: inputRow.sends.indexOf(model.key) >= 0

                        Layout.preferredWidth: page.cellWidth
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 2

                        QQC2.CheckBox {
                            anchors.centerIn: parent
                            visible: page.rank(cell.model.stage) > page.rank(inputRow.model.stage)
                            enabled: Devices.daemonRunning
                            onToggled: Devices.setSend(inputRow.key, cell.model.key, checked)
                            Accessible.name: qsTr("Send %1 to %2").arg(inputRow.model.label).arg(cell.model.label)
                            // Stays bound to the daemon's state after a click.
                            Binding on checked {
                                value: cell.sent
                            }
                        }
                    }
                }
            }
        }
    }
}

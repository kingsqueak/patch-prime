// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

Kirigami.Page {
    id: page

    signal openDevices()
    signal openRouting()
    signal openApps()

    title: qsTr("Mixer")
    header: EngineMessage {}

    actions: [
        Kirigami.Action {
            text: qsTr("Apps")
            icon.name: "applications-multimedia"
            onTriggered: page.openApps()
        },
        Kirigami.Action {
            text: qsTr("Routing")
            icon.name: "network-connect"
            onTriggered: page.openRouting()
        },
        Kirigami.Action {
            text: qsTr("Devices")
            icon.name: "audio-card"
            onTriggered: page.openDevices()
        }
    ]

    StripFilter {
        id: inputs
        sourceModel: Devices
        direction: "input"
    }

    StripFilter {
        id: outputs
        sourceModel: Devices
        direction: "output"
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: Devices.daemonRunning && inputs.count === 0 && outputs.count === 0
        icon.name: "view-media-equalizer"
        text: qsTr("No devices in the mixer")
        explanation: qsTr("Choose the inputs and outputs Patch Prime should use.")
        helpfulAction: Kirigami.Action {
            text: qsTr("Choose Devices…")
            icon.name: "audio-card"
            onTriggered: page.openDevices()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing
        visible: inputs.count > 0 || outputs.count > 0

        Repeater {
            model: [
                { title: qsTr("Inputs"), strips: inputs },
                { title: qsTr("Outputs"), strips: outputs },
            ]

            delegate: ColumnLayout {
                required property var modelData

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: modelData.strips.count > 0
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    level: 3
                    text: modelData.title
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    orientation: ListView.Horizontal
                    spacing: Kirigami.Units.largeSpacing
                    clip: true
                    model: modelData.strips
                    QQC2.ScrollBar.horizontal: QQC2.ScrollBar {}

                    delegate: ChannelStrip {
                        height: ListView.view.height
                    }
                }
            }
        }
    }
}

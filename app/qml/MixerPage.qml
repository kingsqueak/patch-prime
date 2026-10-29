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

    // Inputs and outputs side by side, like a console; scrolls sideways
    // when there are more strips than fit.
    Flickable {
        anchors.fill: parent
        visible: inputs.count > 0 || outputs.count > 0
        contentWidth: consoleRow.implicitWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        QQC2.ScrollBar.horizontal: QQC2.ScrollBar {}

        RowLayout {
            id: consoleRow

            height: parent.height
            spacing: Style.groupGap

            StripGroup {
                Layout.fillHeight: true
                visible: count > 0
                title: qsTr("Inputs")
                strips: inputs
            }

            Kirigami.Separator {
                Layout.fillHeight: true
                visible: inputs.count > 0 && outputs.count > 0
            }

            StripGroup {
                Layout.fillHeight: true
                visible: count > 0
                title: qsTr("Outputs")
                strips: outputs
            }
        }
    }
}

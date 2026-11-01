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
    signal openRecordings()

    title: qsTr("Mixer")
    header: EngineMessage {}

    actions: [
        Kirigami.Action {
            text: qsTr("Recordings")
            icon.name: "media-record"
            onTriggered: page.openRecordings()
        },
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
        stages: ["input"]
    }

    StripFilter {
        id: buses
        sourceModel: Devices
        stages: ["bus"]
    }

    StripFilter {
        id: outputs
        sourceModel: Devices
        stages: ["output"]
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: Devices.daemonRunning && inputs.count === 0 && buses.count === 0 && outputs.count === 0
        icon.name: "view-media-equalizer"
        text: qsTr("No devices in the mixer")
        explanation: qsTr("Choose the inputs and outputs Patch Prime should use.")
        helpfulAction: Kirigami.Action {
            text: qsTr("Choose Devices…")
            icon.name: "audio-card"
            onTriggered: page.openDevices()
        }
    }

    // Scenes: the whole mix saved under a name and loaded back.
    SceneBar {
        id: sceneBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: inputs.count > 0 || buses.count > 0 || outputs.count > 0
    }

    // Inputs, mix buses and outputs side by side, like a console, then the
    // recordings; scrolls
    // sideways when there are more strips than fit. The scroll bar has its
    // own row under the strips, so it never covers them.
    Flickable {
        id: console_

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: scroller.visible ? scroller.top : parent.bottom
        anchors.bottomMargin: scroller.visible ? Style.gap : 0
        anchors.top: sceneBar.visible ? sceneBar.bottom : parent.top
        anchors.topMargin: sceneBar.visible ? Style.gap * 2 : 0
        visible: inputs.count > 0 || buses.count > 0 || outputs.count > 0
        contentWidth: consoleRow.implicitWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        QQC2.ScrollBar.horizontal: scroller

        RowLayout {
            id: consoleRow

            height: parent.height
            spacing: Style.groupGap

            StripGroup {
                Layout.fillHeight: true
                visible: count > 0
                title: qsTr("Inputs")
                stage: "input"
                strips: inputs
            }

            Kirigami.Separator {
                Layout.fillHeight: true
                visible: inputs.count > 0 && appGroup.count > 0
            }

            // Running apps that play sound, as strips of their own.
            AppStripGroup {
                id: appGroup
                Layout.fillHeight: true
                visible: count > 0
            }

            Kirigami.Separator {
                Layout.fillHeight: true
                visible: (inputs.count > 0 || appGroup.count > 0) && (buses.count > 0 || outputs.count > 0)
            }

            StripGroup {
                Layout.fillHeight: true
                visible: count > 0
                title: qsTr("Mix Buses")
                stage: "bus"
                strips: buses
            }

            Kirigami.Separator {
                Layout.fillHeight: true
                visible: buses.count > 0 && outputs.count > 0
            }

            StripGroup {
                Layout.fillHeight: true
                visible: count > 0
                title: qsTr("Outputs")
                stage: "output"
                strips: outputs
            }

            Kirigami.Separator {
                Layout.fillHeight: true
            }

            RecordingsPanel {
                Layout.fillHeight: true
            }
        }
    }

    QQC2.ScrollBar {
        id: scroller

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        orientation: Qt.Horizontal
        policy: QQC2.ScrollBar.AlwaysOn
        visible: console_.visible && console_.contentWidth > console_.width
    }
}

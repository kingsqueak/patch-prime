// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A running app as a mixer strip, in the same zones as ChannelStrip: its
// name and icon, where it plays (the Apps page's route), its meter, and a
// fader and mute on the app's own volume (what Plasma's volume applet
// shows; WirePlumber remembers it per app). Right-click to hide it from
// the mixer.
//
// Looked up by key in Devices.apps, so the strip isn't rebuilt (and a drag
// isn't broken) whenever the app's state changes.
QQC2.Control {
    id: strip

    required property string appKey
    readonly property var app: Devices.apps.find(a => a.key === appKey) || ({})
    readonly property string label: app.name || appKey
    readonly property bool known: app.volumeDb !== undefined && app.volumeDb !== null
    readonly property real volumeDb: known ? app.volumeDb : 0
    readonly property bool muted: app.muted === true
    readonly property var meter: Devices.appLevels[appKey] || ({})
    readonly property var levels: meter.levels || []
    readonly property var peakHold: meter.peakHold || []
    readonly property string target: app.target || ""

    implicitWidth: Style.stripWidth
    padding: Style.stripPadding
    topPadding: Style.stripPadding + Style.stageBarHeight

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        border.color: Style.stripBorder
        border.width: 1
        radius: Style.stripRadius

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: Style.stageBarHeight
            radius: Style.stripRadius
            color: Style.appColor
        }

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: stripMenu.popup()
        }
    }

    QQC2.Menu {
        id: stripMenu

        QQC2.MenuItem {
            text: qsTr("Hide from the Mixer")
            icon.name: "view-hidden"
            onTriggered: Devices.setAppHidden(strip.appKey, true)
        }
    }

    contentItem: ColumnLayout {
        spacing: Style.gap

        // --- header
        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.headerHeight
            Layout.maximumHeight: Style.headerHeight
            spacing: 0

            QQC2.Label {
                Layout.fillWidth: true
                text: strip.label
                font.bold: true
                elide: Text.ElideRight
                QQC2.ToolTip.text: strip.label
                QQC2.ToolTip.visible: truncated && labelHover.hovered
                HoverHandler { id: labelHover }
            }

            QQC2.Label {
                Layout.fillWidth: true
                font: Kirigami.Theme.smallFont
                color: Style.dimText
                elide: Text.ElideRight
                text: (strip.app.streams || 0) > 1 ? qsTr("App, %1 streams").arg(strip.app.streams) : qsTr("App")
            }

            Item { Layout.fillHeight: true }
        }

        // --- the app's icon, where a strip has its inserts
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.insertSlots * Style.slotHeight + (Style.insertSlots - 1) * Style.slotGap

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Math.min(parent.height, Kirigami.Units.iconSizes.large)
                height: width
                source: (strip.app.icon || "").length > 0 ? strip.app.icon : "application-x-executable"
                fallback: "application-x-executable"
                opacity: strip.muted ? 0.4 : 1
            }
        }

        // --- where it plays (as on the Apps page)
        QQC2.Button {
            id: route

            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight
            enabled: Devices.daemonRunning
            text: "→ " + (strip.target.length === 0 ? qsTr("Default")
                          : (Devices.playbackTargets.find(t => t.name === strip.target) || { label: strip.target }).label)
            onClicked: routeMenu.popup(route, 0, route.height)
            QQC2.ToolTip.text: qsTr("Where %1 plays").arg(strip.label)
            QQC2.ToolTip.visible: hovered && !routeMenu.visible
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

            QQC2.Menu {
                id: routeMenu

                QQC2.MenuItem {
                    text: qsTr("System default output")
                    checkable: true
                    checked: strip.target.length === 0
                    onTriggered: Devices.setAppTarget(strip.appKey, "")
                }

                Instantiator {
                    model: Devices.playbackTargets

                    delegate: QQC2.MenuItem {
                        required property var modelData
                        text: modelData.label
                        checkable: true
                        checked: strip.target === modelData.name
                        onTriggered: Devices.setAppTarget(strip.appKey, modelData.name)
                    }

                    onObjectAdded: (index, object) => routeMenu.insertItem(index + 1, object)
                    onObjectRemoved: (index, object) => routeMenu.removeItem(object)
                }
            }
        }

        // --- body: meters and fader
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Style.gap

            Item { Layout.fillWidth: true }

            Row {
                Layout.fillHeight: true
                spacing: Style.meterGap

                Repeater {
                    model: Math.max(strip.levels.length, 2)

                    delegate: LevelMeter {
                        required property int index
                        height: parent.height
                        level: index < strip.levels.length ? strip.levels[index] : 0
                        hold: index < strip.peakHold.length ? strip.peakHold[index] : 0
                        dimmed: strip.muted
                    }
                }
            }

            ConsoleFader {
                Layout.fillHeight: true
                accessibleName: qsTr("Level for %1").arg(strip.label)
                volumeDb: strip.volumeDb
                enabled: Devices.daemonRunning && strip.known
                onRequested: db => Devices.setAppVolume(strip.appKey, db)
                onPressedChanged: readout.dragging = pressed
                onValueChanged: readout.draggedDb = value
            }

            Item { Layout.fillWidth: true }
        }

        // --- readout
        DbField {
            id: readout

            property bool dragging: false
            property real draggedDb: 0

            Layout.fillWidth: true
            font.family: "monospace"
            enabled: strip.known
            value: dragging ? draggedDb : strip.volumeDb
            accessibleName: qsTr("Level for %1 in dB").arg(strip.label)
            onRequested: db => Devices.setAppVolume(strip.appKey, db)
        }

        // --- toggles
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredHeight: Style.controlHeight
            spacing: 0

            StripToggle {
                letter: qsTr("M", "mute button")
                onColor: Style.muteOn
                text: qsTr("Mute")
                on: strip.muted
                enabled: Devices.daemonRunning && strip.known
                onRequested: on => Devices.setAppMuted(strip.appKey, on)
            }
        }

        // --- options: empty, so the faders line up with the channel strips'
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight
        }
    }
}

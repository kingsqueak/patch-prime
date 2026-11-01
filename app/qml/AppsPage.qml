// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The application matrix: where each app plays to and records from. Routes
// are remembered per app and come back when it restarts.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Apps")

    header: EngineMessage {}

    // Running apps first, then by name.
    function appsFor(direction) {
        return Devices.apps.filter(a => a.direction === direction).sort((a, b) => {
            if ((a.streams > 0) !== (b.streams > 0))
                return a.streams > 0 ? -1 : 1;
            return a.name.localeCompare(b.name);
        });
    }

    ColumnLayout {
        spacing: 0

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 4
            visible: Devices.apps.length === 0
            icon.name: "applications-multimedia"
            text: Devices.daemonRunning ? qsTr("No apps playing or recording yet")
                                        : qsTr("Waiting for the engine")
            explanation: qsTr("Apps show up here once they play or record sound, and are remembered.")
        }

        Kirigami.ListSectionHeader {
            Layout.fillWidth: true
            visible: playbackApps.count > 0
            text: qsTr("Playback: where each app plays to")
        }

        Repeater {
            id: playbackApps
            model: page.appsFor("playback")
            delegate: AppRow {
                targets: Devices.playbackTargets
                defaultLabel: qsTr("System default output")
            }
        }

        Kirigami.ListSectionHeader {
            Layout.fillWidth: true
            visible: recordApps.count > 0
            text: qsTr("Recording: where each app records from")
        }

        Repeater {
            id: recordApps
            model: page.appsFor("record")
            delegate: AppRow {
                targets: Devices.recordTargets
                defaultLabel: qsTr("System default input")
            }
        }
    }

    component AppRow: QQC2.ItemDelegate {
        id: row

        required property var modelData
        required property var targets
        required property string defaultLabel
        readonly property bool running: modelData.streams > 0

        // "System default", the strips, and the current target if it's none
        // of those (a device chosen in Plasma, or a strip that's disabled).
        readonly property var choices: {
            const list = [{ name: "", label: defaultLabel }].concat(targets);
            const target = modelData.target;
            if (target.length > 0 && !list.some(t => t.name === target))
                list.push({ name: target, label: qsTr("%1 (not a Patch Prime strip, or off)").arg(target) });
            return list;
        }

        Layout.fillWidth: true
        hoverEnabled: false
        down: false

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                source: row.modelData.icon.length > 0 ? row.modelData.icon : "application-x-executable"
                fallback: "application-x-executable"
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
                opacity: row.running ? 1 : 0.4
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                QQC2.Label {
                    Layout.fillWidth: true
                    text: row.modelData.name
                    elide: Text.ElideRight
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    text: row.running ? qsTr("Running") : qsTr("Not running; the route applies when it starts")
                }
            }

            QQC2.ComboBox {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                model: row.choices
                textRole: "label"
                valueRole: "name"
                enabled: Devices.daemonRunning
                currentIndex: row.choices.findIndex(t => t.name === row.modelData.target)
                onActivated: Devices.setAppTarget(row.modelData.key, currentValue)
                Accessible.name: qsTr("Route for %1").arg(row.modelData.name)
            }

            // Playback apps get a strip in the mixer while they run.
            QQC2.ToolButton {
                visible: row.modelData.direction === "playback"
                icon.name: row.modelData.hidden ? "view-hidden" : "view-visible"
                text: row.modelData.hidden ? qsTr("Show in Mixer") : qsTr("Hide from Mixer")
                display: QQC2.AbstractButton.IconOnly
                enabled: Devices.daemonRunning
                onClicked: Devices.setAppHidden(row.modelData.key, !row.modelData.hidden)
                QQC2.ToolTip.text: row.modelData.hidden ? qsTr("Hidden from the mixer: click to show its strip")
                                                        : qsTr("Shown in the mixer while it plays: click to hide")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }

            QQC2.ToolButton {
                icon.name: "edit-delete-remove"
                text: qsTr("Forget")
                display: QQC2.AbstractButton.IconOnly
                enabled: !row.running
                opacity: row.running ? 0 : 1
                onClicked: Devices.forgetApp(row.modelData.key)
                QQC2.ToolTip.text: qsTr("Forget this app and its route")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// One mixer channel: label, a meter per channel, fader, level and mute.
// Roles are read through `model` (see the note in DevicesPage.qml).
QQC2.Frame {
    id: strip

    required property var model
    readonly property string key: model.key
    readonly property string label: model.label
    readonly property bool online: model.online
    readonly property bool muted: model.muted
    readonly property string direction: model.direction
    readonly property bool busIsDefault: model.busIsDefault
    readonly property string kind: model.kind
    readonly property bool mono: model.mono
    readonly property real volumeDb: model.volumeDb
    readonly property var levels: model.levels
    readonly property int channels: Math.max(1, model.channels)

    width: Kirigami.Units.gridUnit * 6

    function formatDb(db) {
        if (db <= Devices.minimumVolumeDb)
            return "−∞";
        const text = Math.abs(db).toFixed(1) + " dB";
        return db > 0.05 ? "+" + text : db < -0.05 ? "−" + text : "0.0 dB";
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: strip.label
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: !strip.online
            text: qsTr("Offline")
            color: Kirigami.Theme.neutralTextColor
            horizontalAlignment: Text.AlignHCenter
            font: Kirigami.Theme.smallFont
        }

        RowLayout {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            spacing: Kirigami.Units.smallSpacing

            // Meters: one bar per channel, 60 dB scale.
            Row {
                Layout.fillHeight: true
                spacing: 2

                Repeater {
                    model: strip.levels.length > 0 ? strip.levels.length : strip.channels

                    delegate: Rectangle {
                        required property int index
                        readonly property real level: index < strip.levels.length
                                                      ? strip.levels[index] : 0

                        width: Kirigami.Units.smallSpacing * 2
                        height: parent.height
                        radius: 2
                        color: Kirigami.Theme.alternateBackgroundColor
                        border.color: Kirigami.Theme.disabledTextColor
                        border.width: 1

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.width - 2
                            height: (parent.height - 2) * parent.level
                            radius: 1
                            color: parent.level > 0.95 ? Kirigami.Theme.negativeTextColor
                                 : parent.level > 0.8 ? Kirigami.Theme.neutralTextColor
                                 : Kirigami.Theme.positiveTextColor
                            opacity: strip.muted ? 0.4 : 1
                        }
                    }
                }
            }

            QQC2.Slider {
                id: fader

                Layout.fillHeight: true
                orientation: Qt.Vertical
                from: Devices.minimumVolumeDb
                to: Devices.maximumVolumeDb
                enabled: Devices.daemonRunning
                // Half-dB steps, rounded here: a stepSize makes Breeze draw ticks.
                onMoved: Devices.setVolume(strip.key, Math.round(value * 2) / 2)
                Accessible.name: qsTr("Level for %1").arg(strip.label)

                // Follow the daemon, except while the user is dragging.
                Binding on value {
                    when: !fader.pressed
                    value: strip.volumeDb
                    restoreMode: Binding.RestoreNone
                }

                TapHandler {
                    onDoubleTapped: Devices.setVolume(strip.key, 0)
                }

                QQC2.ToolTip.text: qsTr("Double-click for 0 dB")
                QQC2.ToolTip.visible: hovered && !pressed
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: strip.formatDb(fader.pressed ? fader.value : strip.volumeDb)
            font.family: "monospace"
        }

        // Clicking a checkable button breaks a plain `checked:` binding, so the
        // daemon's state is bound with Binding elements, which stay in force.
        QQC2.Button {
            id: muteButton
            Layout.alignment: Qt.AlignHCenter
            checkable: true
            icon.name: strip.muted ? "audio-volume-muted" : "audio-volume-high"
            text: qsTr("Mute")
            enabled: Devices.daemonRunning
            onToggled: Devices.setMuted(strip.key, checked)
            Binding on checked {
                value: strip.muted
            }
        }

        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            visible: strip.direction === "input"
            checkable: true
            icon.name: "audio-input-microphone"
            text: qsTr("Mono")
            enabled: Devices.daemonRunning
            onToggled: Devices.setMono(strip.key, checked)
            Binding on checked {
                value: strip.mono
            }
            QQC2.ToolTip.text: qsTr("Mix all of this input's channels into one, heard in both ears. For a microphone on one input of a stereo interface.")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }

        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            // Anything apps play into: outputs, and virtual inputs. A virtual
            // source is a microphone, not something to play into.
            readonly property bool ownVirtual: strip.kind !== "hardware"
            visible: (strip.direction === "output" && strip.kind !== "virtual-source")
                     || strip.kind === "virtual-input"
            // A virtual device has no hardware to bypass to, so it stays the
            // default until another strip takes over.
            checkable: !(ownVirtual && strip.busIsDefault)
            icon.name: strip.busIsDefault ? "starred-symbolic" : "non-starred-symbolic"
            text: qsTr("Default")
            enabled: Devices.daemonRunning && strip.online
            onToggled: Devices.setDefaultOutput(strip.key, checked)
            Binding on checked {
                value: strip.busIsDefault
            }
            QQC2.ToolTip.text: strip.busIsDefault && ownVirtual
                ? qsTr("Apps that use the system default output play into this strip. Make another strip the default to change that.")
                : strip.busIsDefault
                ? qsTr("Apps that use the system default output play through this strip. Click to send them straight to the device instead.")
                : qsTr("Make this the system default output, so apps play through this strip.")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
}

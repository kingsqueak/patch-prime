// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// One mixer channel. Every strip has the same sections, top to bottom, each
// a fixed height except the body, so strips line up side by side:
//
//   header   name, and a status line (offline, virtual, ...)
//   body     meters + fader (fills the height)
//   readout  fader level in dB
//   toggles  mute, mono, default
//   sends    where it goes (inputs and mix buses; blank on outputs)
//   options  the channel pair choice (blank when there's none)
//
// Sizes and colors come from Style. Roles are read through `model` (see the
// note in DevicesPage.qml).
QQC2.Control {
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
    readonly property bool signal: model.signal
    readonly property int channels: Math.max(1, model.channels)
    readonly property int channelPair: model.channelPair
    readonly property string stage: model.stage
    readonly property var sends: model.sends

    readonly property bool ownVirtual: kind !== "hardware"
    readonly property bool isInput: direction === "input"
    // Anything apps play into can be the default output: outputs other than
    // a virtual microphone, and virtual inputs.
    readonly property bool canBeDefault: (!isInput && kind !== "virtual-source")
                                         || kind === "virtual-input"
    readonly property bool hasPairs: kind === "hardware" && channels > 2

    function formatDb(db) {
        if (db <= Devices.minimumVolumeDb)
            return "−∞";
        const text = Math.abs(db).toFixed(1) + " dB";
        return db > 0.05 ? "+" + text : db < -0.05 ? "−" + text : "0.0 dB";
    }

    implicitWidth: Style.stripWidth
    padding: Style.stripPadding

    background: Rectangle {
        color: Style.stripBackground
        border.color: Style.stripBorder
        radius: Style.stripRadius
    }

    contentItem: ColumnLayout {
        spacing: Style.gap

        // --- header
        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.headerHeight
            Layout.maximumHeight: Style.headerHeight
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: Style.gap

                // Signal light: lit while audio passes, even too quiet to meter.
                Rectangle {
                    implicitWidth: Style.signalSize
                    implicitHeight: Style.signalSize
                    radius: width / 2
                    color: strip.signal ? Style.signalOn : Style.signalOff
                    QQC2.ToolTip.text: strip.signal ? qsTr("Audio is passing through")
                                                    : qsTr("No audio")
                    QQC2.ToolTip.visible: signalHover.hovered
                    HoverHandler { id: signalHover }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: strip.label
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    QQC2.ToolTip.text: strip.label
                    QQC2.ToolTip.visible: truncated && labelHover.hovered
                    HoverHandler { id: labelHover }
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                font: Kirigami.Theme.smallFont
                elide: Text.ElideRight
                color: strip.online ? Style.dimText : Style.offlineText
                text: !strip.online ? qsTr("Offline")
                    : strip.kind === "virtual-input" ? qsTr("Virtual input")
                    : strip.kind === "virtual-source" ? qsTr("Virtual mic")
                    : strip.hasPairs && strip.channelPair > 0
                      ? qsTr("Ch %1–%2").arg(strip.channelPair * 2 - 1).arg(strip.channelPair * 2)
                    : ""
            }

            Item { Layout.fillHeight: true }
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
                    model: strip.levels.length > 0 ? strip.levels.length : strip.channels

                    delegate: LevelMeter {
                        required property int index
                        height: parent.height
                        level: index < strip.levels.length ? strip.levels[index] : 0
                        dimmed: strip.muted
                    }
                }
            }

            VolumeFader {
                Layout.fillHeight: true
                label: strip.label
                volumeDb: strip.volumeDb
                enabled: Devices.daemonRunning
                onRequested: db => Devices.setVolume(strip.key, db)
                onPressedChanged: readout.dragging = pressed
                onValueChanged: readout.draggedDb = value
            }

            Item { Layout.fillWidth: true }
        }

        // --- readout
        QQC2.Label {
            id: readout

            property bool dragging: false
            property real draggedDb: 0

            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            font.family: "monospace"
            text: strip.formatDb(dragging ? draggedDb : strip.volumeDb)
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
                enabled: Devices.daemonRunning
                onRequested: on => Devices.setMuted(strip.key, on)
            }

            StripToggle {
                visible: strip.isInput
                icon.name: "audio-input-microphone"
                text: qsTr("Mono")
                tip: qsTr("Mono: mix all of this input's channels into one, heard in both ears. For a microphone on one input of a stereo interface.")
                on: strip.mono
                enabled: Devices.daemonRunning
                onRequested: on => Devices.setMono(strip.key, on)
            }

            StripToggle {
                visible: strip.canBeDefault
                icon.name: strip.busIsDefault ? "starred-symbolic" : "non-starred-symbolic"
                text: qsTr("Default")
                // A virtual device has no hardware to bypass to, so it stays
                // the default until another strip takes over.
                checkable: !(strip.ownVirtual && strip.busIsDefault)
                on: strip.busIsDefault
                enabled: Devices.daemonRunning && strip.online
                onRequested: on => Devices.setDefaultOutput(strip.key, on)
                tip: strip.busIsDefault && strip.ownVirtual
                     ? qsTr("The system default output. Make another strip the default to change that.")
                     : strip.busIsDefault
                     ? qsTr("The system default output: apps play through this strip. Click to send them straight to the device instead.")
                     : qsTr("Make this the system default output, so apps play through this strip.")
            }
        }

        // --- sends
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight

            SendButton {
                anchors.fill: parent
                visible: strip.stage !== "output"
                enabled: Devices.daemonRunning
                key: strip.key
                stage: strip.stage
                sends: strip.sends
            }
        }

        // --- options
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight

            QQC2.ComboBox {
                anchors.fill: parent
                visible: strip.hasPairs
                enabled: Devices.daemonRunning
                model: {
                    const list = [qsTr("All %1 ch").arg(strip.channels)];
                    for (let pair = 1; pair * 2 <= strip.channels; ++pair)
                        list.push(qsTr("Ch %1–%2").arg(pair * 2 - 1).arg(pair * 2));
                    return list;
                }
                currentIndex: strip.channelPair
                onActivated: index => Devices.setChannelPair(strip.key, index)
                Accessible.name: qsTr("Channels of %1").arg(strip.label)
                QQC2.ToolTip.text: qsTr("Use every channel of this device, or only one stereo pair (for example the front headphone jack of a card in pro-audio mode).")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }
}

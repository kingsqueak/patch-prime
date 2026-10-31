// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// One mixer channel, laid out like a console strip. Every strip has the same
// zones, top to bottom, each a fixed height except the body, so strips line
// up side by side:
//
//   bar      the stage's color (inputs, mix buses, outputs)
//   header   name, and a status line (offline, virtual, channels)
//   inserts  the strip's effects as slots (click to edit)
//   sends    where it goes (inputs and mix buses; blank on outputs)
//   body     meters (RMS, peak, hold; loudness under them on mix buses and
//            outputs), the dB scale and the fader (fills the height)
//   readout  fader level in dB (click to type one)
//   toggles  mute, cue, mono, default
//   options  the channel pair choice; record (and the time recorded) on mix
//            buses and routed inputs
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
    readonly property var rms: model.rms || []
    readonly property var peakHold: model.peakHold || []
    readonly property real lufs: model.lufs === undefined ? -70 : model.lufs
    readonly property bool signal: model.signal
    readonly property int channels: Math.max(1, model.channels)
    readonly property int channelPair: model.channelPair
    readonly property string stage: model.stage
    readonly property var sends: model.sends
    readonly property var effects: model.effects || []
    readonly property bool cued: model.cued === true
    // Mix buses and routed inputs (Desktop) can be recorded.
    readonly property bool canRecord: kind === "virtual-sink" || kind === "virtual-input"
    readonly property bool recording: Devices.recording[key] !== undefined

    readonly property bool ownVirtual: kind !== "hardware"
    readonly property bool isInput: direction === "input"
    // Anything apps play into can be the default output: outputs other than
    // a virtual microphone, and virtual inputs.
    readonly property bool canBeDefault: (!isInput && kind !== "virtual-source")
                                         || kind === "virtual-input"
    // What apps record from: hardware inputs (through their bus), and a
    // virtual microphone.
    readonly property bool canBeDefaultInput: (isInput && kind === "hardware")
                                              || kind === "virtual-source"
    readonly property bool hasPairs: kind === "hardware" && channels > 2

    implicitWidth: Style.stripWidth
    padding: Style.stripPadding
    topPadding: Style.stripPadding + Style.stageBarHeight

    // A card in the View colors (lighter than the window in a light theme,
    // darker in a dark one), with the stage's color along the top.
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        border.color: strip.cued ? Style.cueOn : Style.stripBorder
        border.width: strip.cued ? 2 : 1
        radius: Style.stripRadius

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: Style.stageBarHeight
            radius: Style.stripRadius
            color: Style.stageColor(strip.stage)
            opacity: strip.online ? 1 : 0.4
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
                    elide: Text.ElideRight
                    QQC2.ToolTip.text: strip.label
                    QQC2.ToolTip.visible: truncated && labelHover.hovered
                    HoverHandler { id: labelHover }
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                font: Kirigami.Theme.smallFont
                elide: Text.ElideRight
                color: strip.online ? Style.dimText : Style.offlineText
                text: !strip.online ? qsTr("Offline")
                    : strip.kind === "virtual-input" ? qsTr("Virtual input")
                    : strip.kind === "virtual-source" ? qsTr("Virtual mic")
                    : strip.kind === "virtual-sink" ? qsTr("Mix bus")
                    : strip.hasPairs && strip.channelPair > 0
                      ? qsTr("Ch %1–%2").arg(strip.channelPair * 2 - 1).arg(strip.channelPair * 2)
                    : strip.channels === 1 || strip.mono ? qsTr("Mono") : qsTr("Stereo")
            }

            Item { Layout.fillHeight: true }
        }

        // --- inserts
        InsertSlots {
            Layout.fillWidth: true
            key: strip.key
            label: strip.label
            effects: strip.effects
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

        // --- body: meters, scale and fader
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Style.gap

            Item { Layout.fillWidth: true }

            ColumnLayout {
                Layout.fillHeight: true
                spacing: Style.gap

                Row {
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignHCenter
                    spacing: Style.meterGap

                    Repeater {
                        model: strip.levels.length > 0 ? strip.levels.length : strip.channels

                        delegate: LevelMeter {
                            required property int index
                            height: parent.height
                            level: index < strip.levels.length ? strip.levels[index] : 0
                            rms: index < strip.rms.length ? strip.rms[index] : 0
                            hold: index < strip.peakHold.length ? strip.peakHold[index] : 0
                            dimmed: strip.muted
                        }
                    }
                }

                // Short-term loudness, for streaming targets.
                QQC2.Label {
                    Layout.alignment: Qt.AlignHCenter
                    visible: strip.stage !== "input"
                    text: strip.lufs > -69 ? strip.lufs.toFixed(0) : "–"
                    font: Kirigami.Theme.smallFont
                    color: Style.dimText
                    QQC2.ToolTip.text: qsTr("Short-term loudness (LUFS, last 3 s). Streams usually aim for −14 to −16, podcasts −16, broadcast −23.")
                    QQC2.ToolTip.visible: lufsHover.hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    HoverHandler { id: lufsHover }
                }
            }

            ConsoleFader {
                Layout.fillHeight: true
                accessibleName: qsTr("Level for %1").arg(strip.label)
                volumeDb: strip.volumeDb
                enabled: Devices.daemonRunning
                onRequested: db => Devices.setVolume(strip.key, db)
                onPressedChanged: readout.dragging = pressed
                onValueChanged: readout.draggedDb = value
            }

            Item { Layout.fillWidth: true }
        }

        // --- readout (click to type a level)
        DbField {
            id: readout

            property bool dragging: false
            property real draggedDb: 0

            Layout.fillWidth: true
            font.family: "monospace"
            value: dragging ? draggedDb : strip.volumeDb
            accessibleName: qsTr("Level for %1 in dB").arg(strip.label)
            onRequested: db => Devices.setVolume(strip.key, db)
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
                // Cue: hear this strip alone in the headphones.
                visible: Devices.cueOutput.length > 0 && Devices.cueOutput !== strip.key
                letter: qsTr("C", "cue button")
                onColor: Style.cueOn
                text: qsTr("Cue")
                tip: qsTr("Cue: hear this strip on %1 without changing the mix. While anything is cued, %1 plays only the cued strips.").arg(Devices.labelFor(Devices.cueOutput))
                on: strip.cued
                enabled: Devices.daemonRunning
                onRequested: on => Devices.setCue(strip.key, on)
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
                // Default output for what apps play into; default input
                // (microphone) for what they record from.
                readonly property string what: strip.canBeDefault ? qsTr("output") : qsTr("input (microphone)")
                readonly property string verb: strip.canBeDefault ? qsTr("play through") : qsTr("record")

                visible: strip.canBeDefault || strip.canBeDefaultInput
                icon.name: strip.busIsDefault ? "starred-symbolic" : "non-starred-symbolic"
                text: qsTr("Default")
                // A virtual device has no hardware to bypass to, so it stays
                // the default until another strip takes over.
                checkable: !(strip.ownVirtual && strip.busIsDefault)
                on: strip.busIsDefault
                enabled: Devices.daemonRunning && strip.online
                onRequested: on => strip.canBeDefault ? Devices.setDefaultOutput(strip.key, on)
                                                      : Devices.setDefaultInput(strip.key, on)
                tip: strip.busIsDefault && strip.ownVirtual
                     ? qsTr("The system default %1. Make another strip the default to change that.").arg(what)
                     : strip.busIsDefault
                     ? qsTr("The system default %1: apps %2 this strip. Click to use the device directly instead.").arg(what).arg(verb)
                     : qsTr("Make this the system default %1, so apps %2 this strip.").arg(what).arg(verb)
            }
        }

        // --- options
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight

            // Record (mix buses and Desktop): the time while recording.
            QQC2.ToolButton {
                id: recordButton

                anchors.fill: parent
                anchors.margins: 1
                visible: strip.canRecord
                enabled: Devices.daemonRunning && strip.online
                checkable: true
                text: strip.recording ? Style.clock(Devices.recordingSeconds[strip.key] || 0)
                                      : qsTr("Record")
                onToggled: checked ? Devices.startRecording(strip.key) : Devices.stopRecording(strip.key)
                Binding on checked {
                    value: strip.recording
                }

                // Drawn here: the desktop style draws a button's label in its
                // background, which this replaces.
                contentItem: Item {
                    RowLayout {
                        anchors.centerIn: parent
                        spacing: Kirigami.Units.smallSpacing

                        Rectangle {
                            // A dot to record; a square to stop.
                            implicitWidth: Style.signalSize + 2
                            implicitHeight: implicitWidth
                            radius: strip.recording ? 1 : width / 2
                            color: strip.recording ? Style.toggleOnText : Style.recordOn
                        }
                        QQC2.Label {
                            text: recordButton.text
                            font.family: strip.recording ? "monospace" : Kirigami.Theme.smallFont.family
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            color: strip.recording ? Style.toggleOnText : Kirigami.Theme.textColor
                        }
                    }
                }
                background: Rectangle {
                    radius: Style.toggleRadius
                    color: strip.recording ? Style.recordOn
                                           : recordButton.hovered ? Style.toggleHover : Style.toggleOff
                    opacity: recordButton.enabled ? 1 : 0.5
                }
                Accessible.name: strip.recording ? qsTr("Stop recording %1").arg(strip.label)
                                                 : qsTr("Record %1").arg(strip.label)
                QQC2.ToolTip.text: strip.recording
                     ? qsTr("Recording to %1. Click to stop.").arg(Devices.recording[strip.key])
                     : qsTr("Record what this strip sends on (after its effects and fader) to a %1 file in %2.")
                       .arg(Devices.recordingFormat.toUpperCase()).arg(Devices.recordingFolder)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }

            QQC2.ComboBox {
                anchors.fill: parent
                visible: strip.hasPairs
                enabled: Devices.daemonRunning
                font: Kirigami.Theme.smallFont
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

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The player's transport: play/pause, loop, stop and what's playing; under
// it the position (drag to jump) and the time.
ColumnLayout {
    id: controls

    required property RecordingsState rec

    spacing: Kirigami.Units.smallSpacing

    component Tool: QQC2.ToolButton {
        display: QQC2.AbstractButton.IconOnly
        QQC2.ToolTip.text: text
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 0

        Tool {
            readonly property bool going: controls.rec.playing.length > 0 && !controls.rec.paused
            icon.name: going ? "media-playback-pause" : "media-playback-start"
            text: going ? qsTr("Pause") : qsTr("Play")
            enabled: controls.rec.playing.length > 0 || controls.rec.files.length > 0
            onClicked: controls.rec.playPause()
        }
        Tool {
            icon.name: "media-playlist-repeat"
            text: qsTr("Loop: play the recording over and over, for example while you set up effects")
            checkable: true
            checked: controls.rec.looping
            onToggled: Devices.setPlaybackLoop(checked)
        }
        Tool {
            icon.name: "media-playback-stop"
            text: qsTr("Stop")
            enabled: controls.rec.playing.length > 0
            onClicked: Devices.stopPlayback()
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.smallSpacing
            elide: Text.ElideMiddle
            text: controls.rec.playing.length > 0 ? controls.rec.playing.split("/").pop()
                                                  : qsTr("Nothing playing")
            opacity: controls.rec.playing.length > 0 ? 1 : 0.6
        }
    }

    RowLayout {
        Layout.fillWidth: true

        QQC2.Slider {
            id: position
            Layout.fillWidth: true
            from: 0
            to: Math.max(controls.rec.duration, 0.001)
            enabled: controls.rec.playing.length > 0
            // Follows the player, except while dragged; a drag seeks when let
            // go (see VolumeFader.qml).
            onPressedChanged: {
                if (!pressed)
                    Devices.seekPlayback(value);
            }
            Accessible.name: qsTr("Position")
        }
        Connections {
            target: Devices
            function onRecordingTimesChanged() {
                if (!position.pressed)
                    position.value = Devices.playerPosition;
            }
        }
        QQC2.Label {
            font.family: "monospace"
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            text: qsTr("%1 / %2").arg(Style.clock(position.pressed ? position.value : Devices.playerPosition))
                                 .arg(Style.clock(controls.rec.duration))
        }
    }
}

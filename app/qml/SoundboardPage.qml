// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The soundboard: pads that play sound files into one strip (Stream Mix,
// say). Add sounds, choose where they play, click a pad to play it. Pads
// can also be played from MIDI, hotkeys and patchprime-ctl.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Soundboard")

    actions: [
        Kirigami.Action {
            text: qsTr("Add Sounds…")
            icon.name: "list-add"
            enabled: Devices.daemonRunning
            onTriggered: fileDialog.open()
        },
        Kirigami.Action {
            text: qsTr("Stop All")
            icon.name: "media-playback-stop"
            enabled: Devices.pads.some(p => p.playing)
            onTriggered: Devices.stopAllPads()
        }
    ]

    FileDialog {
        id: fileDialog
        title: qsTr("Add Sounds")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Sound files (*.wav *.flac *.mp3 *.ogg *.oga *.opus *.aiff *.aif)"), qsTr("All files (*)")]
        onAccepted: {
            for (const file of selectedFiles)
                Devices.addPad(file);
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.FormLayout {
            Layout.fillWidth: true

            QQC2.ComboBox {
                id: target
                Kirigami.FormData.label: qsTr("Plays into:")
                Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                enabled: Devices.daemonRunning
                textRole: "label"
                valueRole: "key"
                model: [{ key: "", label: qsTr("Choose a strip…") }].concat(
                    Devices.playbackTargets.map(t => ({ key: t.key, label: t.label })))
                currentIndex: Math.max(0, model.findIndex(m => m.key === Devices.soundboardTarget))
                onActivated: Devices.setSoundboardTarget(currentValue)
            }

            QQC2.Label {
                Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                text: qsTr("A mix bus (your stream) or Desktop (heard everywhere Desktop goes). Click a pad to play it, again to stop it; pads can play at once. Right-click a pad to rename it, set its level or loop it, or play it from a MIDI control or a hotkey.")
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 2
            visible: Devices.pads.length === 0
            icon.name: "media-playlist-play"
            text: qsTr("No sounds yet")
            explanation: qsTr("Add sound files (WAV, FLAC, MP3, Ogg) to play them on cue.")
            helpfulAction: Kirigami.Action {
                text: qsTr("Add Sounds…")
                icon.name: "list-add"
                onTriggered: fileDialog.open()
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Repeater {
                model: Devices.pads

                delegate: PadButton {
                    width: Kirigami.Units.gridUnit * 9
                    height: Kirigami.Units.gridUnit * 3.5
                }
            }
        }
    }
}

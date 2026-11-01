// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A soundboard pad: click to play it, again to stop it. Lit while it plays,
// with how far it's got along the bottom. Right-click to edit or remove
// it, or give it a MIDI control or a hotkey.
QQC2.Button {
    id: pad

    required property var modelData // {id, label, path, gainDb, loop, playing}
    readonly property bool playing: modelData.playing === true
    readonly property real progress: Devices.padProgress[modelData.id] || 0

    text: modelData.label
    icon.name: playing ? "media-playback-stop" : (modelData.loop ? "media-repeat-all" : "media-playback-start")
    highlighted: playing
    enabled: Devices.daemonRunning
    onClicked: Devices.triggerPad(modelData.id)
    QQC2.ToolTip.text: playing ? qsTr("Playing: click to stop") : (modelData.loop ? qsTr("Click to play (loops until clicked again)") : qsTr("Click to play"))
    QQC2.ToolTip.visible: hovered
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 2
        height: 3
        radius: 1
        width: (parent.width - 4) * Math.min(1, pad.progress)
        visible: pad.playing
        color: Kirigami.Theme.highlightedTextColor
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: menu.popup()
    }

    MidiLearnMenu {
        id: menu
        target: ({ action: "pad", pad: pad.modelData.id })

        QQC2.MenuSeparator {}

        QQC2.MenuItem {
            text: qsTr("Edit Pad…")
            icon.name: "document-edit"
            onTriggered: {
                const host = pad.QQC2.ApplicationWindow.window;
                padDialog.createObject(host ? host.contentItem : pad, { pad: pad.modelData }).open();
            }
        }

        QQC2.MenuItem {
            text: qsTr("Remove Pad")
            icon.name: "edit-delete-remove"
            onTriggered: Devices.removePad(pad.modelData.id)
        }
    }

    Component {
        id: padDialog
        PadDialog {}
    }
}

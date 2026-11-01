// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The right-click menu of a control a MIDI controller can drive: learn a
// control for it, or forget the one it has. `target` is what it is
// ({action, key, destination, effect, param, scene}; see StartMidiLearn).
// Put it in the control and call popup() from a right-click.
QQC2.Menu {
    id: menu

    required property var target
    readonly property var mapping: Devices.midiMappingFor(target)
    readonly property bool mapped: mapping.id !== undefined

    QQC2.MenuItem {
        text: Devices.midiController.connected ? qsTr("MIDI Learn")
              : Devices.midiController.id ? qsTr("MIDI Learn (the MIDI controller isn't connected)")
              : qsTr("MIDI Learn (choose a MIDI controller on the Devices page)")
        icon.name: "audio-midi"
        enabled: Devices.daemonRunning && Devices.midiController.connected === true
        onTriggered: Devices.startMidiLearn(menu.target)
    }

    QQC2.MenuItem {
        visible: menu.mapped
        height: visible ? implicitHeight : 0
        text: qsTr("Forget MIDI Control (%1)").arg(menu.mapping.control || "")
        icon.name: "edit-delete-remove"
        onTriggered: Devices.removeMidiMapping(menu.mapping.id)
    }
}

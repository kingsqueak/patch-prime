// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The right-click menu of a control that hardware can drive: learn a MIDI
// control for it, forget the one it has, or give it a global hotkey. `target` is what it is
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

    QQC2.MenuSeparator {}

    QQC2.MenuItem {
        // Hotkeys can't set a parameter (only switch one).
        visible: menu.target.action !== "effectParam"
        height: visible ? implicitHeight : 0
        text: Devices.hotkeysAvailable ? qsTr("Add Hotkey…") : qsTr("Add Hotkey (no global shortcuts on this desktop)")
        icon.name: "input-keyboard"
        enabled: Devices.daemonRunning && Devices.hotkeysAvailable
        onTriggered: {
            // On the window, not the control: a control in a list can be
            // rebuilt (and its children with it) while the dialog is open.
            const host = menu.parent.QQC2.ApplicationWindow.window;
            hotkeyDialog.createObject(host ? host.contentItem : menu.parent,
                                      { fixedTarget: menu.target, destroyOnClose: true }).open();
        }
    }

    Component {
        id: hotkeyDialog
        HotkeyDialog {}
    }
}

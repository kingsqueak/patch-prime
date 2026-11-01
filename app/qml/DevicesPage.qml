// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls
import org.kingsqueak.patchprime.engine

Kirigami.ScrollablePage {
    id: page

    title: qsTr("Devices")

    header: EngineMessage {}

    actions: Kirigami.Action {
        text: qsTr("Add Virtual Device…")
        icon.name: "list-add"
        enabled: Devices.daemonRunning
        onTriggered: addDialog.open()
    }

    Kirigami.PromptDialog {
        id: addDialog

        title: qsTr("Add Virtual Device")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onOpened: {
            nameField.text = "";
            nameField.forceActiveFocus();
        }
        onAccepted: {
            if (nameField.text.trim().length > 0)
                Devices.createVirtual(nameField.text, inputButton.checked ? "input"
                                                    : sinkButton.checked ? "sink" : "source");
        }

        ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: inputButton.checked ? qsTr("e.g. Desktop")
                               : sinkButton.checked ? qsTr("e.g. Stream Mix") : qsTr("e.g. Chat Mic")
                onAccepted: addDialog.accept()
            }

            QQC2.RadioButton {
                id: inputButton
                checked: true
                text: qsTr("Input: apps play into it, and you route it to outputs (desktop sound on several headphones)")
            }

            QQC2.RadioButton {
                id: sinkButton
                text: qsTr("Mix bus: inputs and apps mix into it, it goes to outputs, and apps can record it (a stream mix for OBS)")
            }

            QQC2.RadioButton {
                text: qsTr("Microphone: apps record from it (a mic for Discord)")
            }
        }
    }

    // One hardware device's own settings. Applying them restarts WirePlumber,
    // which takes a few seconds and briefly interrupts all audio.
    Kirigami.Dialog {
        id: deviceDialog

        property string key
        property int rate
        property string scheduling

        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        padding: Kirigami.Units.largeSpacing
        onAccepted: {
            Devices.setDeviceSampleRate(key, rateChoice.currentValue);
            Devices.setDeviceScheduling(key, schedulingChoice.currentValue);
        }

        Kirigami.FormLayout {
            QQC2.ComboBox {
                id: rateChoice
                Kirigami.FormData.label: qsTr("Sample rate:")
                textRole: "text"
                valueRole: "value"
                model: [{ value: 0, text: qsTr("Same as the mixer") }].concat(
                    [44100, 48000, 88200, 96000].map(r => ({ value: r, text: qsTr("%1 kHz").arg(r / 1000) })))
                currentIndex: Math.max(0, model.findIndex(m => m.value === deviceDialog.rate))
            }

            QQC2.ComboBox {
                id: schedulingChoice
                Kirigami.FormData.label: qsTr("Scheduling:")
                textRole: "text"
                valueRole: "value"
                model: [
                    { value: "auto", text: qsTr("Automatic") },
                    { value: "timer", text: qsTr("Timer") },
                    { value: "interrupt", text: qsTr("Interrupt") },
                ]
                currentIndex: Math.max(0, model.findIndex(m => m.value === deviceDialog.scheduling))
            }

            QQC2.Label {
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                text: qsTr("Timer scheduling lets a device follow another clock smoothly; Automatic uses it while Patch Prime is the clock. Interrupt suits a device that is the clock itself. Changing these restarts WirePlumber: audio stops for a few seconds.")
            }
        }
    }

    ListView {
        id: list

        model: Devices

        // Settings for the whole mixer, above the device list.
        // Settings for the whole mixer, above the device list.
        header: ColumnLayout {
            width: ListView.view.width
            spacing: 0

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                text: qsTr("Audio engine")
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing

                // Patch Prime's own clock, or one interface as the master clock.
                QQC2.ComboBox {
                    id: clockChoice
                    Kirigami.FormData.label: qsTr("Clock:")
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    enabled: Devices.daemonRunning
                    textRole: "text"
                    valueRole: "value"
                    model: Devices.clockChoices
                    currentIndex: Devices.clock === "hardware"
                                  ? model.findIndex(c => c.value === Devices.clockDevice) : 0
                    displayText: currentIndex >= 0 ? currentText : qsTr("Patch Prime")
                    onActivated: {
                        if (currentValue === "internal")
                            Devices.setClock("internal");
                        else
                            Devices.setClockDevice(currentValue);
                    }
                }

                QQC2.Label {
                    Kirigami.FormData.label: qsTr("Clock now:")
                    text: Devices.clockNow
                }

                QQC2.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: Devices.clock === "hardware"
                        ? qsTr("That interface is the master clock: the others follow it on timer scheduling. If it's unplugged, Patch Prime keeps time until it's back, with a brief glitch on the other interfaces as the clock changes hands.")
                        : qsTr("Patch Prime keeps time itself, so unplugging any interface doesn't interrupt the others. Interfaces follow it with PipeWire's rate matching.")
                }

                QQC2.ComboBox {
                    Kirigami.FormData.label: qsTr("Sample rate:")
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    enabled: Devices.daemonRunning
                    textRole: "text"
                    valueRole: "value"
                    model: [44100, 48000, 88200, 96000, 176400, 192000].map(r => ({
                        value: r, text: qsTr("%1 kHz").arg(r / 1000) }))
                    currentIndex: Math.max(0, model.findIndex(m => m.value === Devices.sampleRate))
                    onActivated: Devices.setSampleRate(currentValue)
                }

                QQC2.ComboBox {
                    Kirigami.FormData.label: qsTr("Block size:")
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    enabled: Devices.daemonRunning
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: 0, text: qsTr("Automatic") }].concat(
                        [64, 128, 256, 512, 1024, 2048].map(f => ({
                            value: f,
                            text: qsTr("%1 frames (%2 ms)").arg(f)
                                  .arg((f * 1000 / Math.max(Devices.sampleRate, 1)).toFixed(1)) })))
                    currentIndex: Math.max(0, model.findIndex(m => m.value === Devices.blockSize))
                    onActivated: Devices.setBlockSize(currentValue)
                }
            }

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                text: qsTr("Controllers")
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing

                // A Mackie Control (or a controller in its Mackie mode), by
                // its MIDI port; the chosen one stays listed while unplugged.
                QQC2.ComboBox {
                    readonly property var surface: Devices.controlSurface
                    Kirigami.FormData.label: qsTr("Mackie Control:")
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    enabled: Devices.daemonRunning && surface.available === true
                    textRole: "text"
                    valueRole: "value"
                    model: {
                        const ports = (surface.ports || []).map(p => ({ value: p.id, text: p.name }));
                        const chosen = surface.id || "";
                        if (chosen !== "" && !ports.some(p => p.value === chosen))
                            ports.push({ value: chosen,
                                         text: qsTr("%1 (unplugged)").arg(chosen.slice(chosen.indexOf(":") + 1)) });
                        return [{ value: "", text: qsTr("None") }].concat(ports);
                    }
                    currentIndex: Math.max(0, model.findIndex(m => m.value === (surface.id || "")))
                    onActivated: Devices.setControlSurface(currentValue)
                }

                QQC2.Label {
                    Kirigami.FormData.label: qsTr("Status:")
                    visible: (Devices.controlSurface.id || "") !== "" || Devices.controlSurface.available !== true
                    text: Devices.controlSurface.available !== true ? qsTr("No MIDI (the ALSA sequencer isn't available)")
                        : Devices.controlSurface.connected ? qsTr("Connected") : qsTr("Not connected: plug it in")
                }

                QQC2.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("Faders, MUTE, SOLO (cue) and REC work on eight strips at a time (BANK and CHANNEL move them); SELECT a mix bus or output, and the V-Pots set each strip's send to it. F1 to F8 load your first eight scenes.")
                }

                // Any other MIDI controller: its controls are taught with
                // MIDI Learn on the app's controls.
                QQC2.ComboBox {
                    readonly property var surface: Devices.controlSurface
                    Kirigami.FormData.label: qsTr("MIDI controller:")
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    enabled: Devices.daemonRunning && surface.available === true
                    textRole: "text"
                    valueRole: "value"
                    model: {
                        const ports = (surface.ports || []).map(p => ({ value: p.id, text: p.name }));
                        const chosen = Devices.midiController.id || "";
                        if (chosen !== "" && !ports.some(p => p.value === chosen))
                            ports.push({ value: chosen,
                                         text: qsTr("%1 (unplugged)").arg(chosen.slice(chosen.indexOf(":") + 1)) });
                        return [{ value: "", text: qsTr("None") }].concat(ports);
                    }
                    currentIndex: Math.max(0, model.findIndex(m => m.value === (Devices.midiController.id || "")))
                    onActivated: Devices.setMidiController(currentValue)
                }

                QQC2.Label {
                    Kirigami.FormData.label: qsTr("Status:")
                    visible: (Devices.midiController.id || "") !== ""
                    text: Devices.midiController.connected ? qsTr("Connected") : qsTr("Not connected: plug it in")
                }

                QQC2.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: qsTr("To use a control on it, right-click a fader, button, send or effect setting in Patch Prime, choose MIDI Learn, then move the control.")
                }
            }

            // What the MIDI controller's controls do.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                Layout.bottomMargin: Kirigami.Units.largeSpacing
                visible: Devices.midiMappings.length > 0
                spacing: 0

                QQC2.Label {
                    text: qsTr("MIDI mappings")
                    font.bold: true
                }

                Repeater {
                    model: Devices.midiMappings

                    delegate: RowLayout {
                        id: mappingRow

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing

                        QQC2.Label {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                            text: mappingRow.modelData.control
                            color: Kirigami.Theme.disabledTextColor
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: mappingRow.modelData.description
                            elide: Text.ElideRight
                        }

                        QQC2.CheckBox {
                            visible: mappingRow.modelData.kind === "cc"
                            text: qsTr("Endless encoder")
                            checked: mappingRow.modelData.relative
                            onToggled: Devices.setMidiMappingRelative(mappingRow.modelData.id, checked)
                            QQC2.ToolTip.text: qsTr("For a knob that turns forever and sends up/down steps (1–63 up, 65–127 down) instead of a position.")
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                        }

                        QQC2.ToolButton {
                            icon.name: "edit-delete-remove"
                            text: qsTr("Forget")
                            display: QQC2.AbstractButton.IconOnly
                            onClicked: Devices.removeMidiMapping(mappingRow.modelData.id)
                            QQC2.ToolTip.text: qsTr("Forget this mapping")
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                        }
                    }
                }
            }

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                text: qsTr("Hotkeys")
            }

            // Global hotkeys: their keys are recorded here (or in System
            // Settings → Shortcuts).
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    text: Devices.hotkeysAvailable
                        ? qsTr("Keys that work in every app: mute or push to talk on your mic, duck the music, load a scene. Also from a control's right-click menu.")
                        : qsTr("Global hotkeys need KDE's shortcut service (kglobalaccel), which isn't running.")
                }

                Repeater {
                    model: Devices.hotkeys

                    delegate: RowLayout {
                        id: hotkeyRow

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: hotkeyRow.modelData.description
                            elide: Text.ElideRight
                        }

                        KQuickControls.KeySequenceItem {
                            keySequence: Devices.keySequence(hotkeyRow.modelData.keys.length > 0 ? hotkeyRow.modelData.keys[0] : 0)
                            modifierlessAllowed: true
                            checkForConflictsAgainst: KQuickControls.ShortcutType.GlobalShortcuts | KQuickControls.ShortcutType.StandardShortcuts
                            onKeySequenceModified: Devices.setHotkeyKey(hotkeyRow.modelData.id, keySequence)
                        }

                        QQC2.ToolButton {
                            icon.name: "edit-delete-remove"
                            text: qsTr("Remove")
                            display: QQC2.AbstractButton.IconOnly
                            onClicked: Devices.removeHotkey(hotkeyRow.modelData.id)
                            QQC2.ToolTip.text: qsTr("Remove this hotkey")
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                        }
                    }
                }

                QQC2.Button {
                    text: qsTr("Add Hotkey…")
                    icon.name: "list-add"
                    enabled: Devices.daemonRunning && Devices.hotkeysAvailable
                    onClicked: addHotkeyDialog.open()
                }

                HotkeyDialog {
                    id: addHotkeyDialog
                }
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: list.count === 0
            icon.name: "audio-card"
            text: !Devices.daemonRunning ? qsTr("Waiting for the engine")
                : Devices.connected ? qsTr("No audio devices") : qsTr("Waiting for PipeWire")
        }

        // Section headers are drawn by the delegate, not ListView.section: with
        // Qt 6.10, inserting a row above a section boundary left a delegate
        // showing another row's data.
        delegate: ColumnLayout {
            id: row

            // Role values come through `model`, which follows the row when rows
            // are inserted above it, rather than one required property per role.
            required property var model
            readonly property string key: model.key
            readonly property string customLabel: model.customLabel
            readonly property string description: model.description
            readonly property string nodeName: model.nodeName
            readonly property string direction: model.direction
            readonly property bool isVirtual: model.isVirtual
            readonly property bool online: model.online
            readonly property bool isEnabled: model.isEnabled
            readonly property int channels: model.channels
            readonly property string positions: model.positions
            readonly property bool isDefault: model.isDefault
            readonly property bool saved: model.saved
            readonly property bool sectionStart: model.sectionStart
            readonly property string kind: model.kind
            readonly property bool ownVirtual: kind !== "hardware"
            readonly property int deviceRate: model.deviceRate
            readonly property string scheduling: model.scheduling

            width: ListView.view.width
            spacing: 0

            Kirigami.ListSectionHeader {
                Layout.fillWidth: true
                visible: row.sectionStart
                text: row.direction === "input" ? qsTr("Inputs")
                                                : qsTr("Outputs and virtual devices")
            }

            QQC2.ItemDelegate {
                Layout.fillWidth: true
                hoverEnabled: false
                down: false

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    QQC2.CheckBox {
                        checked: row.isEnabled
                        onToggled: Devices.setEnabled(row.key, checked)
                        QQC2.ToolTip.text: qsTr("Use this device in Patch Prime")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }

                    Kirigami.Icon {
                        source: row.kind === "virtual-input" ? "audio-volume-high"
                              : row.direction === "input" ? "audio-input-microphone"
                              : row.isVirtual ? "audio-volume-high" : "audio-speakers"
                        implicitWidth: Kirigami.Units.iconSizes.medium
                        implicitHeight: Kirigami.Units.iconSizes.medium
                        opacity: row.online ? 1 : 0.4
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        QQC2.TextField {
                            Layout.fillWidth: true
                            text: row.customLabel
                            placeholderText: row.description
                            onEditingFinished: Devices.setLabel(row.key, text)
                            Accessible.name: qsTr("Label for %1").arg(row.description)
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                            text: {
                                const parts = [];
                                if (row.customLabel.length > 0)
                                    parts.push(row.description);
                                parts.push(row.kind === "virtual-input" ? qsTr("Patch Prime virtual input")
                                           : row.kind === "virtual-sink" ? qsTr("Patch Prime mix bus")
                                           : row.kind === "virtual-source" ? qsTr("Patch Prime virtual microphone")
                                           : row.isVirtual ? qsTr("virtual") : qsTr("hardware"));
                                if (row.channels > 0)
                                    parts.push(row.positions.length > 0
                                               ? qsTr("%1 ch (%2)").arg(row.channels).arg(row.positions)
                                               : qsTr("%1 ch").arg(row.channels));
                                parts.push(row.nodeName);
                                return parts.join(" · ");
                            }
                        }
                    }

                    QQC2.Label {
                        visible: row.isDefault
                        text: qsTr("Default")
                        color: Kirigami.Theme.positiveTextColor
                        QQC2.ToolTip.text: row.direction === "input"
                                           ? qsTr("The system's default input")
                                           : qsTr("The system's default output")
                        QQC2.ToolTip.visible: defaultHover.hovered
                        HoverHandler { id: defaultHover }
                    }

                    QQC2.Label {
                        visible: !row.online
                        text: qsTr("Offline")
                        color: Kirigami.Theme.neutralTextColor
                    }

                    QQC2.ToolButton {
                        visible: !row.ownVirtual && row.nodeName.startsWith("alsa_")
                        icon.name: "configure"
                        text: qsTr("Device settings")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: {
                            deviceDialog.key = row.key;
                            deviceDialog.title = row.customLabel.length > 0 ? row.customLabel : row.description;
                            deviceDialog.rate = row.deviceRate;
                            deviceDialog.scheduling = row.scheduling;
                            deviceDialog.open();
                        }
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }

                    QQC2.ToolButton {
                        visible: row.ownVirtual || (!row.online && row.saved)
                        icon.name: "edit-delete-remove"
                        text: row.ownVirtual ? qsTr("Delete") : qsTr("Forget")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: Devices.forget(row.key)
                        QQC2.ToolTip.text: row.ownVirtual ? qsTr("Delete this virtual device")
                                                          : qsTr("Forget this device's settings")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }
            }
        }
    }
}

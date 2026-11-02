// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Multitrack takes, on the Recordings page and the mixer's Recordings panel:
// the multitrack switch (strip REC buttons then arm strips), Record Take
// (every armed strip at once, a file each), and whether inputs are recorded
// raw or after their effects. Compact: smaller, for the panel.
ColumnLayout {
    id: take

    property bool compact: false
    readonly property bool running: Devices.takeFolder.length > 0
    // Armed strips that exist, by label.
    readonly property var armedLabels: {
        const labels = [];
        for (let row = 0; row < strips.count; ++row) {
            const s = strips.get(row);
            if (Devices.armed.includes(s.key))
                labels.push(s.label);
        }
        return labels;
    }

    spacing: Style.gap

    StripFilter {
        id: strips
        sourceModel: Devices
        stages: ["input", "bus", "output"]
    }

    QQC2.Switch {
        Layout.fillWidth: true
        text: qsTr("Multitrack")
        font: take.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
        checked: Devices.multitrack
        enabled: Devices.daemonRunning && !take.running
        onToggled: Devices.setMultitrack(checked)
        QQC2.ToolTip.text: qsTr("Record several strips at once, each to its own file, all starting on the same sample. While on, the strips' Record buttons arm them for the next take instead of recording.")
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Devices.multitrack
        spacing: Style.gap

        QQC2.Button {
            id: takeButton

            Layout.fillWidth: take.compact
            enabled: Devices.daemonRunning && (take.running || take.armedLabels.length > 0)
            font: take.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
            icon.name: take.running ? "media-playback-stop" : "media-record"
            icon.color: take.running ? Kirigami.Theme.textColor : Style.recordOn
            text: take.running ? qsTr("Stop Take %1").arg(Style.clock(Devices.takeSeconds))
                               : qsTr("Record Take")
            onClicked: take.running ? Devices.stopTake() : Devices.startTake()
            QQC2.ToolTip.text: take.running
                ? qsTr("Recording %1 into %2").arg(take.armedLabels.join(", ")).arg(Devices.takeFolder)
                : take.armedLabels.length > 0
                ? qsTr("Record %1, a file each, in a new take folder in %2").arg(take.armedLabels.join(", ")).arg(Devices.recordingFolder)
                : qsTr("Arm strips with their Record buttons first")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

            // Right-click: MIDI Learn and hotkeys.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: takeMidi.popup()
            }
            MidiLearnMenu {
                id: takeMidi
                target: ({ action: "take" })
            }
        }

        QQC2.ComboBox {
            Layout.fillWidth: true
            font: take.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
            model: [
                { value: true, text: qsTr("Inputs raw") },
                { value: false, text: qsTr("Inputs processed") }
            ]
            textRole: "text"
            valueRole: "value"
            currentIndex: Devices.takeInputsRaw ? 0 : 1
            onActivated: Devices.setTakeInputsRaw(currentValue)
            Accessible.name: qsTr("How inputs are recorded in a take")
            QQC2.ToolTip.text: qsTr("Raw: hardware inputs as the device sends them, before their effects, to process later. Processed: after their effects and fader, as the mix hears them. Mix buses are always recorded as mixed.")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        visible: Devices.multitrack
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        opacity: 0.7
        text: take.armedLabels.length > 0
              ? qsTr("Armed: %1").arg(take.armedLabels.join(", "))
              : qsTr("Arm strips with their Record buttons in the mixer.")
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Recording settings and multitrack takes, on the Recordings page and the
// mixer's Recordings panel: whether inputs are recorded raw or after their
// effects, the multitrack switch (strip REC buttons then arm strips), and
// Record Take (every armed strip at once, a file each). Compact: smaller,
// for the panel.
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

    // Always: how inputs are recorded, and multitrack on or off.
    RowLayout {
        Layout.fillWidth: true
        spacing: Style.gap

        QQC2.ComboBox {
            Layout.fillWidth: take.compact
            Layout.preferredWidth: take.compact ? -1 : Kirigami.Units.gridUnit * 12
            font: take.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
            model: [
                { value: true, text: qsTr("Inputs raw") },
                { value: false, text: qsTr("Inputs processed") }
            ]
            textRole: "text"
            valueRole: "value"
            enabled: Devices.daemonRunning
            currentIndex: Devices.recordInputsRaw ? 0 : 1
            onActivated: Devices.setRecordInputsRaw(currentValue)
            Accessible.name: qsTr("How inputs are recorded")
            QQC2.ToolTip.text: qsTr("How a hardware input (a microphone) is recorded, alone or in a take. Raw: as the device sends it, before its effects and fader, to process later or to set up its effects on. Processed: after its effects and fader, as the mix hears it. Mix buses and Desktop are always recorded as mixed.")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }

        QQC2.Switch {
            text: qsTr("Multitrack")
            font: take.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
            checked: Devices.multitrack
            enabled: Devices.daemonRunning && !take.running
            onToggled: Devices.setMultitrack(checked)
            QQC2.ToolTip.text: qsTr("Record several strips at once, each to its own file, all starting on the same sample. While on, the strips' Record buttons arm them for the next take instead of recording.")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }

    QQC2.Button {
        id: takeButton

        Layout.fillWidth: take.compact
        visible: Devices.multitrack
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

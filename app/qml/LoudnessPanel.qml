// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Loudness for publishing, on the Recordings page and the mixer's
// Recordings panel: the strip measured, the target, the integrated loudness
// against it (green within 1 LU, amber within 2, red beyond), and short-term,
// momentary, loudness range, true peak and how long it's measured. It starts
// over with Reset, and by itself when the strip starts recording.
ColumnLayout {
    id: loud

    property bool compact: false

    // [momentary, short-term, integrated, range, true peak, seconds]; -1000 = none yet.
    readonly property var values: Devices.loudness
    function reading(i) {
        return values.length > i && values[i] > -999 ? values[i] : NaN;
    }
    readonly property real integrated: reading(2)
    readonly property real truePeak: reading(4)
    readonly property real seconds: values.length > 5 ? values[5] : 0
    readonly property real offTarget: integrated - Devices.loudnessTarget

    readonly property var presets: [
        { text: qsTr("Podcast (−16 LUFS, −1 dBTP)"), lufs: -16, dbtp: -1 },
        { text: qsTr("Streaming (−14 LUFS, −1 dBTP)"), lufs: -14, dbtp: -1 },
        { text: qsTr("EBU R128 broadcast (−23 LUFS, −1 dBTP)"), lufs: -23, dbtp: -1 },
        { text: qsTr("ATSC A/85 (−24 LUFS, −2 dBTP)"), lufs: -24, dbtp: -2 }
    ]
    readonly property int preset: presets.findIndex(p => p.lufs === Devices.loudnessTarget
                                                         && p.dbtp === Devices.loudnessPeakTarget)

    function number(v, digits) {
        return isNaN(v) ? "—" : (v < 0 ? "−" : "") + Math.abs(v).toFixed(digits);
    }

    spacing: Style.gap

    component Name: QQC2.Label {
        font: Kirigami.Theme.smallFont
        opacity: 0.7
    }
    component Value: QQC2.Label {
        Layout.fillWidth: true
        font.family: "monospace"
        font.pointSize: Kirigami.Theme.smallFont.pointSize
    }

    StripFilter {
        id: strips
        sourceModel: Devices
        stages: ["input", "bus", "output"]
    }

    // What's measured, and Reset.
    RowLayout {
        Layout.fillWidth: true
        spacing: Style.gap

        QQC2.ComboBox {
            id: stripChoice

            readonly property var choices: {
                const list = [{ key: "", label: qsTr("Loudness: off") }];
                for (let row = 0; row < strips.count; ++row) {
                    const s = strips.get(row);
                    list.push({ key: s.key, label: qsTr("Loudness of %1").arg(s.label) });
                }
                return list;
            }

            Layout.fillWidth: loud.compact
            Layout.preferredWidth: loud.compact ? -1 : Kirigami.Units.gridUnit * 16
            font: loud.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
            enabled: Devices.daemonRunning
            model: choices
            textRole: "label"
            valueRole: "key"
            currentIndex: Math.max(0, choices.findIndex(c => c.key === Devices.loudnessStrip))
            onActivated: Devices.setLoudnessStrip(currentValue)
            Accessible.name: qsTr("Strip to measure loudness on")
            QQC2.ToolTip.text: qsTr("Measure this strip's loudness as it sends on (after its effects and fader), the way streaming sites and podcast apps measure it (EBU R128 / ITU-R BS.1770).")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
        QQC2.ToolButton {
            icon.name: "view-refresh"
            text: qsTr("Reset")
            display: loud.compact ? QQC2.AbstractButton.IconOnly : QQC2.AbstractButton.TextBesideIcon
            enabled: Devices.loudnessMeasuring
            onClicked: Devices.resetLoudness()
            QQC2.ToolTip.text: qsTr("Start the measurement over. It also starts over by itself when this strip starts recording, so the reading matches the file.")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }

    // The target.
    RowLayout {
        Layout.fillWidth: true
        visible: Devices.loudnessStrip.length > 0
        spacing: Style.gap

        QQC2.ComboBox {
            Layout.fillWidth: loud.compact
            Layout.preferredWidth: loud.compact ? -1 : Kirigami.Units.gridUnit * 16
            font: loud.compact ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
            model: loud.presets.map(p => p.text).concat([qsTr("Custom")])
            currentIndex: loud.preset >= 0 ? loud.preset : loud.presets.length
            displayText: loud.preset >= 0 ? loud.presets[loud.preset].text
                                          : qsTr("Custom (%1 LUFS, %2 dBTP)").arg(loud.number(Devices.loudnessTarget, 0))
                                                                              .arg(loud.number(Devices.loudnessPeakTarget, 0))
            onActivated: index => {
                if (index < loud.presets.length)
                    Devices.setLoudnessTarget(loud.presets[index].lufs, loud.presets[index].dbtp);
                else
                    customRow.visible = true;
            }
            Accessible.name: qsTr("Loudness target")
        }
    }
    RowLayout {
        id: customRow

        Layout.fillWidth: true
        visible: false
        spacing: Style.gap

        QQC2.SpinBox {
            from: -40
            to: 0
            value: Devices.loudnessTarget
            editable: true
            textFromValue: v => qsTr("%1 LUFS").arg(loud.number(v, 0))
            valueFromText: t => parseInt(t.replace("−", "-"))
            onValueModified: Devices.setLoudnessTarget(value, Devices.loudnessPeakTarget)
            Accessible.name: qsTr("Target loudness")
        }
        QQC2.SpinBox {
            from: -12
            to: 0
            value: Devices.loudnessPeakTarget
            editable: true
            textFromValue: v => qsTr("%1 dBTP").arg(loud.number(v, 0))
            valueFromText: t => parseInt(t.replace("−", "-"))
            onValueModified: Devices.setLoudnessTarget(Devices.loudnessTarget, value)
            Accessible.name: qsTr("Highest true peak")
        }
        QQC2.ToolButton {
            icon.name: "dialog-ok"
            onClicked: customRow.visible = false
            Accessible.name: qsTr("Done")
        }
    }

    // The readings.
    RowLayout {
        Layout.fillWidth: true
        visible: Devices.loudnessStrip.length > 0
        spacing: Kirigami.Units.largeSpacing

        ColumnLayout {
            spacing: 0

            QQC2.Label {
                text: loud.number(loud.integrated, 1)
                font.family: "monospace"
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * (loud.compact ? 1.6 : 2.2)
                font.bold: true
                color: isNaN(loud.integrated) ? Kirigami.Theme.disabledTextColor
                     : Math.abs(loud.offTarget) <= 1 ? Kirigami.Theme.positiveTextColor
                     : Math.abs(loud.offTarget) <= 2 ? Kirigami.Theme.neutralTextColor
                     : Kirigami.Theme.negativeTextColor
                Accessible.name: qsTr("Integrated loudness %1 LUFS").arg(text)
            }
            QQC2.Label {
                font: Kirigami.Theme.smallFont
                opacity: 0.8
                text: !Devices.loudnessMeasuring ? qsTr("Not running")
                    : isNaN(loud.integrated) ? qsTr("LUFS integrated: listening…")
                    : qsTr("LUFS integrated, %1 LU %2 target").arg(loud.number(Math.abs(loud.offTarget), 1))
                                                              .arg(loud.offTarget >= 0 ? qsTr("over") : qsTr("under"))
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Kirigami.Units.smallSpacing
            rowSpacing: 0

            Name { text: qsTr("Short-term") }
            Value { text: qsTr("%1 LUFS").arg(loud.number(loud.reading(1), 1)) }
            Name { text: qsTr("Momentary") }
            Value { text: qsTr("%1 LUFS").arg(loud.number(loud.reading(0), 1)) }
            Name { text: qsTr("Range") }
            Value { text: qsTr("%1 LU").arg(loud.number(loud.reading(3), 1)) }
            Name { text: qsTr("True peak") }
            Value {
                text: qsTr("%1 dBTP").arg(loud.number(loud.truePeak, 1))
                color: loud.truePeak > Devices.loudnessPeakTarget ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
            }
            Name { text: qsTr("Measured") }
            Value { text: Style.clock(loud.seconds) }
        }
    }
}

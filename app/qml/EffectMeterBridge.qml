// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine
import "effectmeters.js" as EffectMeters

// An effect's meters, side by side like a console's meter bridge: the level
// going in (per channel), its gain reduction (dynamics only; the leveler's
// gain both ways) and the level coming out, each with a readout below. The
// levels are measured only while the effects window is open.
RowLayout {
    id: bridge

    property string key
    property int index
    property string type
    property bool on: true

    readonly property var levels: Devices.effectLevels[key + "|" + index] || ({})
    readonly property var own: Devices.effectMeters[key + "|" + index] || []
    readonly property var reduction: EffectMeters.gainReduction(type, own)
    readonly property bool bipolar: EffectMeters.isBipolar(type)

    // A peak (meter fraction) as dBFS text.
    function peakText(fractions) {
        const f = Math.max(0, ...(fractions || [0]));
        return f <= 0 ? "−∞" : ((f - 1) * -Devices.minimumVolumeDb).toFixed(1);
    }

    spacing: Kirigami.Units.smallSpacing

    component MeterColumn: ColumnLayout {
        property string title
        property string readout
        default property alias bars: row.data

        Layout.fillHeight: true
        spacing: 2

        QQC2.Label {
            Layout.alignment: Qt.AlignHCenter
            text: parent.title
            font: Kirigami.Theme.smallFont
            color: Style.dimText
        }
        Row {
            id: row

            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            spacing: Style.meterGap
        }
        QQC2.Label {
            Layout.preferredWidth: Style.fxReadoutWidth
            horizontalAlignment: Text.AlignHCenter
            text: parent.readout
            font.family: "monospace"
            font.pointSize: Kirigami.Theme.smallFont.pointSize
        }
    }

    MeterColumn {
        title: qsTr("IN")
        readout: bridge.peakText(bridge.levels.in)

        Repeater {
            model: Math.max(1, (bridge.levels.in || []).length)

            delegate: LevelMeter {
                required property int index
                implicitWidth: Style.fxMeterWidth
                height: parent ? parent.height : 0
                level: (bridge.levels.in || [])[index] || 0
                rms: (bridge.levels.inRms || [])[index] || 0
                dimmed: !bridge.on
            }
        }
    }

    MeterColumn {
        visible: bridge.reduction !== null
        title: bridge.bipolar ? qsTr("GAIN") : qsTr("GR")
        readout: bridge.bipolar ? (bridge.own[0] || 0).toFixed(1) : "−" + (bridge.reduction || 0).toFixed(1)

        GainReductionMeter {
            height: parent ? parent.height : 0
            db: bridge.bipolar ? (bridge.own[0] || 0) : (bridge.reduction || 0)
            range: EffectMeters.reductionRange(bridge.type)
            bipolar: bridge.bipolar
            dimmed: !bridge.on
        }
    }

    MeterColumn {
        title: qsTr("OUT")
        readout: bridge.peakText(bridge.levels.out)

        Repeater {
            model: Math.max(1, (bridge.levels.out || []).length)

            delegate: LevelMeter {
                required property int index
                implicitWidth: Style.fxMeterWidth
                height: parent ? parent.height : 0
                level: (bridge.levels.out || [])[index] || 0
                rms: (bridge.levels.outRms || [])[index] || 0
                dimmed: !bridge.on
            }
        }
    }
}

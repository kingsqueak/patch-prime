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
// gain both ways) and the level coming out, each with a readout below, on a
// dB scale down to -80 (a mic's noise floor). Where a level is compared with
// a setting, the setting is a line across the meter: the gate's threshold on
// IN, the limiter's ceiling on IN and OUT, and the ducker's threshold on its
// KEY (the strip that turns this one down). The levels are measured only
// while the effects window is open.
RowLayout {
    id: bridge

    property string key
    property int index
    property string type
    property bool on: true
    property var params: ({})   // the effect's values by name
    property var paramInfo: []  // its parameters (for the defaults)

    readonly property var levels: Devices.effectLevels[key + "|" + index] || ({})
    readonly property var own: Devices.effectMeters[key + "|" + index] || []
    readonly property var reduction: EffectMeters.gainReduction(type, own)
    readonly property bool bipolar: EffectMeters.isBipolar(type)

    readonly property real floorDb: Devices.effectMeterFloorDb
    readonly property bool keyed: (levels.key || []).length > 0
    // The line on IN (and OUT), in dB, or NaN for none.
    readonly property real inMark: type === "gate" ? param("threshold")
                                 : type === "limiter" ? param("ceiling") : NaN
    readonly property string inMarkName: type === "limiter" ? qsTr("Ceiling") : qsTr("Threshold")

    // A peak (meter fraction) as dBFS text.
    function peakText(fractions) {
        const f = Math.max(0, ...(fractions || [0]));
        return f <= 0 ? "−∞" : ((f - 1) * -floorDb).toFixed(1);
    }
    // A parameter's value, or its default when it was never set.
    function param(name) {
        const v = params[name];
        if (v !== undefined)
            return v;
        const info = paramInfo.find(p => p.name === name);
        return info ? info.initial : NaN;
    }
    function fractionFor(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / -floorDb));
    }

    spacing: Kirigami.Units.smallSpacing

    component MeterColumn: ColumnLayout {
        id: column

        property string title
        property string readout
        property real markDb: NaN // a setting's line across the meter
        property string markName
        default property alias bars: row.data

        Layout.fillHeight: true
        spacing: 2

        QQC2.Label {
            Layout.alignment: Qt.AlignHCenter
            text: column.title
            font: Kirigami.Theme.smallFont
            color: Style.dimText
        }
        Item {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: row.implicitWidth + 2 * Style.meterMarkOverhang

            Row {
                id: row

                anchors.horizontalCenter: parent.horizontalCenter
                height: parent.height
                spacing: Style.meterGap
            }
            Rectangle {
                visible: !isNaN(column.markDb)
                y: Math.round(parent.height * (1 - bridge.fractionFor(column.markDb)) - height / 2)
                width: parent.width
                height: Style.meterMarkHeight
                radius: 1
                color: Style.meterMark
            }
            HoverHandler {
                id: hover
            }
            QQC2.ToolTip.visible: hover.hovered && !isNaN(column.markDb)
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: qsTr("%1: %2 dB").arg(column.markName).arg(column.markDb.toFixed(1))
        }
        QQC2.Label {
            Layout.preferredWidth: Style.fxReadoutWidth
            horizontalAlignment: Text.AlignHCenter
            text: column.readout
            font.family: "monospace"
            font.pointSize: Kirigami.Theme.smallFont.pointSize
        }
    }

    // The dB scale, beside the meters.
    MeterColumn {
        title: " "
        readout: " "

        Item {
            implicitWidth: Style.fxScaleWidth
            height: parent ? parent.height : 0

            Repeater {
                model: [0, -20, -40, -60]

                delegate: QQC2.Label {
                    required property int modelData
                    anchors.right: parent.right
                    y: Math.round(parent.height * (1 - bridge.fractionFor(modelData)) - height / 2)
                    text: modelData
                    font.pointSize: Kirigami.Theme.smallFont.pointSize * 0.9
                    color: Style.scaleText
                }
            }
        }
    }

    // The ducker's key: what it listens to.
    MeterColumn {
        visible: bridge.keyed
        title: qsTr("KEY")
        readout: bridge.peakText(bridge.levels.key)
        markDb: bridge.type === "ducker" ? bridge.param("threshold") : NaN
        markName: qsTr("Threshold")

        LevelMeter {
            implicitWidth: Style.fxMeterWidth
            height: parent ? parent.height : 0
            level: (bridge.levels.key || [])[0] || 0
            rms: (bridge.levels.keyRms || [])[0] || 0
            dimmed: !bridge.on
        }
    }

    MeterColumn {
        title: qsTr("IN")
        readout: bridge.peakText(bridge.levels.in)
        markDb: bridge.inMark
        markName: bridge.inMarkName

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
        markDb: bridge.type === "limiter" ? bridge.inMark : NaN
        markName: bridge.inMarkName

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

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick

// Gain reduction, as on a compressor: an amber bar hanging down from the
// top, range dB at the bottom, with a tick every quarter. Bipolar (the
// leveler), it's centred on 0 dB: boost rises above the middle, cut hangs
// below it, range dB each way.
Rectangle {
    id: meter

    property real db: 0 // reduction (positive); bipolar: the gain, + or -
    property real range: 24
    property bool bipolar: false
    property bool dimmed: false

    readonly property real fraction: Math.min(1, Math.abs(db) / range) * (bipolar ? 0.5 : 1)

    implicitWidth: Style.fxMeterWidth
    radius: Style.meterRadius
    color: Style.meterBackground

    Rectangle {
        width: parent.width
        height: parent.height * meter.fraction
        y: !meter.bipolar ? 0 : meter.db >= 0 ? parent.height / 2 - height : parent.height / 2
        radius: Style.meterRadius
        color: meter.bipolar && meter.db > 0 ? Style.grBoost : Style.grColor
        opacity: meter.dimmed ? 0.4 : 1
    }

    Repeater {
        model: [0.25, 0.5, 0.75]

        delegate: Rectangle {
            required property real modelData
            y: Math.round(meter.height * modelData)
            width: meter.width
            height: 1
            color: modelData === 0.5 && meter.bipolar ? Style.faderCapBorder : Style.scaleTick
        }
    }
}

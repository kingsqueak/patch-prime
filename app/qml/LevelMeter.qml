// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick

// One channel's level: the RMS as a solid bar, the peak as a lighter bar
// above it, and the highest recent peak as a thin line that holds, then
// falls. All levels are 0..1 of the meter scale.
Rectangle {
    id: meter

    property real level: 0 // peak
    property real rms: 0
    property real hold: 0
    property bool dimmed: false

    function colorFor(value) {
        return value > Style.meterClip ? Style.meterClipColor
             : value > Style.meterWarn ? Style.meterWarnColor
             : Style.meterLow;
    }

    implicitWidth: Style.meterWidth
    radius: Style.meterRadius
    color: Style.meterBackground

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: parent.height * meter.level
        radius: Style.meterRadius
        color: meter.colorFor(meter.level)
        opacity: meter.dimmed ? 0.2 : 0.45
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: parent.height * Math.min(meter.rms, meter.level)
        radius: Style.meterRadius
        color: meter.colorFor(meter.rms)
        opacity: meter.dimmed ? 0.4 : 1
    }

    Rectangle {
        visible: meter.hold > 0.01
        y: parent.height * (1 - meter.hold)
        width: parent.width
        height: 2
        color: meter.colorFor(meter.hold)
        opacity: meter.dimmed ? 0.4 : 1
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick

// One channel's level bar. level: 0..1 of the meter scale.
Rectangle {
    id: meter

    property real level: 0
    property bool dimmed: false

    implicitWidth: Style.meterWidth
    radius: Style.meterRadius
    color: Style.meterBackground

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: parent.height * meter.level
        radius: Style.meterRadius
        color: meter.level > Style.meterClip ? Style.meterClipColor
             : meter.level > Style.meterWarn ? Style.meterWarnColor
             : Style.meterLow
        opacity: meter.dimmed ? 0.4 : 1
    }
}

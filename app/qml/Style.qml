// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

pragma Singleton

import QtQuick
import org.kde.kirigami as Kirigami

// Every size, gap and color the mixer uses, in one place, so the look can be
// changed here without touching the components. The look is a mixing
// console's (strips in zones, lit buttons, fader caps, dB scales), in the
// colors of the Plasma color scheme, light or dark.
QtObject {
    // Layout
    readonly property real gap: Kirigami.Units.smallSpacing
    readonly property real groupGap: Kirigami.Units.gridUnit
    readonly property real stripGap: Math.round(Kirigami.Units.smallSpacing * 1.5)
    readonly property real controlHeight: Math.round(Kirigami.Units.gridUnit * 1.5)
    readonly property real stripPadding: Kirigami.Units.smallSpacing
    // Compact: four toggle buttons wide (mute, cue, mono, default).
    readonly property real stripWidth: controlHeight * 4 + stripPadding * 2
    readonly property real headerHeight: Math.round(Kirigami.Units.gridUnit * 2.2)
    readonly property real stripRadius: Kirigami.Units.cornerRadius
    readonly property color stripBorder: Qt.alpha(Kirigami.Theme.textColor, 0.15)

    // Each stage has a color: a bar on top of its strips and its group's title.
    readonly property color inputColor: Kirigami.Theme.positiveTextColor
    readonly property color busColor: Kirigami.Theme.highlightColor
    readonly property color outputColor: Kirigami.Theme.neutralTextColor
    readonly property real stageBarHeight: 3
    function stageColor(stage) {
        return stage === "input" ? inputColor : stage === "bus" ? busColor : outputColor;
    }

    // Insert slots (a strip's effects)
    readonly property int insertSlots: 4
    readonly property real slotHeight: Math.round(Kirigami.Units.gridUnit * 1.05)
    readonly property real slotGap: 2
    readonly property real slotRadius: 3
    readonly property color slotOn: Kirigami.Theme.highlightColor
    readonly property color slotOff: Qt.alpha(Kirigami.Theme.textColor, 0.07)

    // Meters
    readonly property real meterWidth: Math.round(Kirigami.Units.smallSpacing * 1.5)
    readonly property real meterGap: 2
    readonly property real meterRadius: 1
    readonly property real meterWarn: 0.8   // fraction of the scale where yellow starts
    readonly property real meterClip: 0.95  // and red
    readonly property color meterBackground: Qt.alpha(Kirigami.Theme.textColor, 0.1)
    readonly property color meterLow: Kirigami.Theme.positiveTextColor
    readonly property color meterWarnColor: Kirigami.Theme.neutralTextColor
    readonly property color meterClipColor: Kirigami.Theme.negativeTextColor
    // Effect meters (the effects window's meter bridge): gain reduction in
    // amber, a leveler's boost in green.
    readonly property real fxMeterWidth: Math.round(Kirigami.Units.smallSpacing * 2)
    readonly property real fxReadoutWidth: Math.round(Kirigami.Units.gridUnit * 3.2)
    readonly property real fxBridgeHeight: Kirigami.Units.gridUnit * 7.5
    readonly property real fxScaleWidth: Math.round(Kirigami.Units.gridUnit * 1.3)
    // A setting (threshold, ceiling) as a line across an effect meter.
    readonly property color meterMark: Kirigami.Theme.textColor
    readonly property real meterMarkHeight: 2
    readonly property real meterMarkOverhang: 3
    readonly property color grColor: Kirigami.Theme.neutralTextColor
    readonly property color grBoost: Kirigami.Theme.positiveTextColor
    // An insert slot's gain-reduction bar, along its bottom edge.
    readonly property real slotReductionHeight: 3

    // Faders: a cap on a slim groove, with a dB scale beside it.
    readonly property real faderCapWidth: Math.round(Kirigami.Units.gridUnit * 1.3)
    readonly property real faderCapHeight: Math.round(Kirigami.Units.gridUnit * 1.125)
    readonly property real faderGrooveWidth: 4
    readonly property real faderScaleWidth: Math.round(Kirigami.Units.gridUnit * 1.7)
    readonly property color faderGroove: Qt.alpha(Kirigami.Theme.textColor, 0.22)
    // The fader's slot, shaded to look inset (works on light and dark).
    readonly property real faderSlotMargin: 0 // the slot is exactly as wide as the cap
    readonly property color slotShadeEdge: Qt.rgba(0, 0, 0, 0.16)
    readonly property color slotShadeMid: Qt.rgba(0, 0, 0, 0.07)
    readonly property color slotShadow: Qt.rgba(0, 0, 0, 0.28)
    readonly property color slotHighlight: Qt.rgba(1, 1, 1, 0.22)
    readonly property color slotRim: Qt.rgba(0, 0, 0, 0.18)
    // A lighter line just outside the slot's dark rim: the panel's edge
    // catching the light, so the recess reads on dark schemes too.
    readonly property color slotEdgeLight: Qt.rgba(1, 1, 1, 0.16)
    readonly property color faderCapBorder: Qt.alpha(Kirigami.Theme.textColor, 0.45)
    readonly property color faderCapLine: Kirigami.Theme.highlightColor
    readonly property color scaleText: Kirigami.Theme.disabledTextColor
    readonly property color scaleTick: Qt.alpha(Kirigami.Theme.textColor, 0.25)

    // Text
    readonly property color dimText: Kirigami.Theme.disabledTextColor
    readonly property color offlineText: Kirigami.Theme.neutralTextColor

    // Strip toggles: lit buttons. Off they're faintly filled so they read as
    // buttons; `onColor` is the usual on color, and a toggle can set its own
    // (mute is red, cue is amber).
    readonly property real toggleRadius: 3
    readonly property color toggleOff: Qt.alpha(Kirigami.Theme.textColor, 0.07)
    readonly property color toggleHover: Qt.alpha(Kirigami.Theme.textColor, 0.14)
    readonly property color toggleOn: Kirigami.Theme.highlightColor
    readonly property color toggleOnText: Kirigami.Theme.highlightedTextColor
    readonly property color muteOn: Kirigami.Theme.negativeTextColor
    readonly property color cueOn: Kirigami.Theme.neutralTextColor

    // Signal light: lit while any audio passes, even below the meter's range.
    readonly property real signalSize: Kirigami.Units.smallSpacing * 2
    readonly property color signalOn: Kirigami.Theme.positiveTextColor
    readonly property color signalOff: Qt.alpha(Kirigami.Theme.textColor, 0.15)

    // Effect editors (the EQ graph)
    readonly property real graphHeight: Kirigami.Units.gridUnit * 12
    readonly property color graphBackground: Qt.alpha(Kirigami.Theme.textColor, 0.04)
    readonly property color graphGrid: Qt.alpha(Kirigami.Theme.textColor, 0.12)
    readonly property color graphLabel: Kirigami.Theme.disabledTextColor
    readonly property color graphCurve: Kirigami.Theme.highlightColor
    readonly property real handleSize: Kirigami.Units.gridUnit * 1.1
    // One color per EQ band, so a handle matches its column in the table.
    readonly property var bandColors: ["#e5534b", "#e0823d", "#c9b52c", "#3fae5c", "#3d8fd6", "#9a6ad6"]
}

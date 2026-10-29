// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

pragma Singleton

import QtQuick
import org.kde.kirigami as Kirigami

// Every size, gap and color the mixer uses, in one place, so the look can be
// changed here without touching the components. Colors follow the Plasma
// color scheme.
QtObject {
    // Layout
    readonly property real gap: Kirigami.Units.smallSpacing
    readonly property real groupGap: Kirigami.Units.gridUnit
    readonly property real stripWidth: Kirigami.Units.gridUnit * 5
    readonly property real stripPadding: Kirigami.Units.smallSpacing
    readonly property real headerHeight: Kirigami.Units.gridUnit * 2.5
    readonly property real controlHeight: Kirigami.Units.gridUnit * 1.75
    readonly property real stripRadius: Kirigami.Units.cornerRadius

    // Meters
    readonly property real meterWidth: Kirigami.Units.smallSpacing * 1.5
    readonly property real meterGap: 2
    readonly property real meterRadius: 1
    readonly property real meterWarn: 0.8   // fraction of the scale where yellow starts
    readonly property real meterClip: 0.95  // and red

    // Colors
    readonly property color stripBackground: Kirigami.Theme.backgroundColor
    readonly property color stripBorder: Qt.alpha(Kirigami.Theme.textColor, 0.15)
    readonly property color meterBackground: Qt.alpha(Kirigami.Theme.textColor, 0.08)
    readonly property color meterLow: Kirigami.Theme.positiveTextColor
    readonly property color meterWarnColor: Kirigami.Theme.neutralTextColor
    readonly property color meterClipColor: Kirigami.Theme.negativeTextColor
    readonly property color dimText: Kirigami.Theme.disabledTextColor

    // Strip toggles: background when off, hovered, and on. `onColor` is the
    // usual on color; a toggle can set its own (mute is red).
    readonly property real toggleRadius: Kirigami.Units.cornerRadius
    readonly property color toggleOff: "transparent"
    readonly property color toggleHover: Qt.alpha(Kirigami.Theme.textColor, 0.1)
    readonly property color toggleOn: Kirigami.Theme.highlightColor
    readonly property color toggleOnText: Kirigami.Theme.highlightedTextColor
    readonly property color muteOn: Kirigami.Theme.negativeTextColor

    // Signal light: lit while any audio passes, even below the meter's range.
    readonly property real signalSize: Kirigami.Units.smallSpacing * 2
    readonly property color signalOn: Kirigami.Theme.positiveTextColor
    readonly property color signalOff: Qt.alpha(Kirigami.Theme.textColor, 0.15)
    readonly property color offlineText: Kirigami.Theme.neutralTextColor
}

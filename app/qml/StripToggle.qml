// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// A compact on/off button for a strip: an icon (or a letter, like "M"), its
// name in the tooltip, and a colored background while on (Style.toggleOn,
// or onColor).
//
// `on` is the daemon's state. Clicking a checkable button breaks a plain
// `checked:` binding, so it's bound with a Binding element, which stays in
// force; `requested(on)` asks the daemon for the change.
QQC2.ToolButton {
    id: button

    property bool on: false
    property string tip
    property string letter // shown instead of the icon when set
    property color onColor: Style.toggleOn

    signal requested(bool on)

    implicitWidth: Style.controlHeight
    implicitHeight: Style.controlHeight
    display: letter.length > 0 ? QQC2.AbstractButton.TextOnly : QQC2.AbstractButton.IconOnly
    checkable: true
    onToggled: requested(checked)

    Binding on checked {
        value: button.on
    }

    contentItem: Item {
        Kirigami.Icon {
            anchors.centerIn: parent
            visible: button.letter.length === 0
            width: Kirigami.Units.iconSizes.small
            height: width
            source: button.icon.name
            color: button.checked ? Style.toggleOnText : Kirigami.Theme.textColor
            isMask: button.checked
        }

        QQC2.Label {
            anchors.centerIn: parent
            visible: button.letter.length > 0
            text: button.letter
            font.bold: true
            color: button.checked ? Style.toggleOnText : Kirigami.Theme.textColor
        }
    }

    // A lit button: faintly filled when off (so it reads as a button), in
    // its color when on. A little inset, so neighbours don't touch.
    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Style.toggleRadius
            color: button.checked ? button.onColor : button.hovered ? Style.toggleHover : Style.toggleOff
            opacity: button.enabled ? 1 : 0.5
        }
    }

    QQC2.ToolTip.text: tip.length > 0 ? tip : text
    QQC2.ToolTip.visible: hovered
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
}

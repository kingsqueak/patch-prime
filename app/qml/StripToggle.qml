// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// A compact on/off button for a strip: an icon, its name in the tooltip.
//
// `on` is the daemon's state. Clicking a checkable button breaks a plain
// `checked:` binding, so it's bound with a Binding element, which stays in
// force; `requested(on)` asks the daemon for the change.
QQC2.ToolButton {
    id: button

    property bool on: false
    property string tip

    signal requested(bool on)

    implicitWidth: Style.controlHeight
    implicitHeight: Style.controlHeight
    display: QQC2.AbstractButton.IconOnly
    checkable: true
    onToggled: requested(checked)

    Binding on checked {
        value: button.on
    }

    QQC2.ToolTip.text: tip.length > 0 ? tip : text
    QQC2.ToolTip.visible: hovered
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
}

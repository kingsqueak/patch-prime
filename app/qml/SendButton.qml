// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Where a strip sends its audio, like a console's bus assign: the button
// names the destination (or how many), and its menu ticks them on and off.
// An input can send to mix buses and outputs; a mix bus to outputs.
QQC2.Button {
    id: button

    property string key
    property string stage // the strip's own: "input" or "bus"
    property var sends: []

    text: sends.length === 0 ? qsTr("No sends")
        : sends.length === 1 ? "→ " + Devices.labelFor(sends[0])
        : qsTr("→ %1 places").arg(sends.length)
    icon.name: "go-next-symbolic"
    display: QQC2.AbstractButton.TextOnly
    onClicked: menu.popup(button, 0, button.height)

    QQC2.ToolTip.text: sends.length === 0 ? qsTr("Choose where this strip goes")
        : sends.map(k => Devices.labelFor(k)).join("\n")
    QQC2.ToolTip.visible: hovered && !menu.visible
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

    StripFilter {
        id: destinations
        sourceModel: Devices
        stages: button.stage === "input" ? ["bus", "output"] : ["output"]
    }

    QQC2.Menu {
        id: menu

        Instantiator {
            model: destinations

            delegate: QQC2.MenuItem {
                required property var model
                text: model.stage === "bus" ? qsTr("%1 (mix bus)").arg(model.label) : model.label
                checkable: true
                checked: button.sends.indexOf(model.key) >= 0
                onTriggered: Devices.setSend(button.key, model.key, checked)
            }

            onObjectAdded: (index, object) => menu.insertItem(index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }

        QQC2.MenuItem {
            enabled: false
            visible: destinations.count === 0
            text: qsTr("Nothing to send to: enable an output")
        }
    }
}

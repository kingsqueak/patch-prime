// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's effects as insert slots, like on a console: a short name per
// effect in chain order, lit while on and dim while bypassed. Click one (or
// the empty slot) to open the strip's effects window. Always the same
// height, so strips line up: with more effects than slots, the last slot
// says how many more.
ColumnLayout {
    id: inserts

    property string key
    property string label
    property var effects: []

    // Short names for a narrow strip.
    readonly property var shortNames: ({
        eq: qsTr("EQ"), multiband: qsTr("Multiband"), gate: qsTr("Gate"), ducker: qsTr("Ducker"),
        denoise: qsTr("Denoise"), leveler: qsTr("Leveler"), limiter: qsTr("Limiter"),
        reverb: qsTr("Reverb"), delay: qsTr("Delay"), gain: qsTr("Gain")
    })
    readonly property int shown: effects.length <= Style.insertSlots ? effects.length
                                                                       : Style.insertSlots - 1

    function open() {
        window.open();
    }

    spacing: Style.slotGap
    Layout.preferredHeight: Style.insertSlots * Style.slotHeight + (Style.insertSlots - 1) * Style.slotGap

    // Developer aid (PATCHPRIME_EFFECTS): open this strip's window at start.
    onLabelChanged: {
        if (label.length > 0 && QQC2.ApplicationWindow.window
                && QQC2.ApplicationWindow.window.openEffects === label)
            window.open();
    }

    component Slot: QQC2.AbstractButton {
        id: slot

        property bool lit: false
        property bool placeholder: false

        Layout.fillWidth: true
        Layout.preferredHeight: Style.slotHeight
        hoverEnabled: true
        onClicked: inserts.open()

        contentItem: QQC2.Label {
            text: slot.text
            font: Kirigami.Theme.smallFont
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: slot.lit ? Style.toggleOnText
                 : slot.placeholder ? Style.dimText : Kirigami.Theme.textColor
        }
        background: Rectangle {
            radius: Style.slotRadius
            color: slot.lit ? Style.slotOn : slot.hovered ? Style.toggleHover : Style.slotOff
            border.color: slot.placeholder ? Style.stripBorder : "transparent"
            border.width: 1
        }

        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    Repeater {
        model: inserts.shown

        delegate: Slot {
            required property int index
            readonly property var effect: inserts.effects[index]

            text: inserts.shortNames[effect.type] || effect.type
            lit: effect.enabled
            QQC2.ToolTip.text: (effect.enabled ? qsTr("%1 (on)") : qsTr("%1 (bypassed)"))
                               .arg(inserts.shortNames[effect.type] || effect.type)
                               + "\n" + qsTr("Click to edit the effects")
        }
    }

    // More effects than slots.
    Slot {
        visible: inserts.effects.length > Style.insertSlots
        text: qsTr("+%1 more").arg(inserts.effects.length - inserts.shown)
        placeholder: true
        QQC2.ToolTip.text: qsTr("Click to see all the effects")
    }

    // The empty slot: add the first effect.
    Slot {
        visible: inserts.effects.length === 0
        text: qsTr("+ Effects")
        placeholder: true
        enabled: Devices.daemonRunning
        QQC2.ToolTip.text: qsTr("Add effects to %1: EQ, compressor, gate, reverb…").arg(inserts.label)
    }

    Item {
        Layout.fillHeight: true
    }

    EffectsWindow {
        id: window
        key: inserts.key
        label: inserts.label
        effects: inserts.effects
    }
}

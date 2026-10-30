// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's fader, drawn like a console's: a dB scale on the left, a slot
// the width of the cap that looks cut into the panel (shaded darker at its
// edges, a shadow under its top edge, a highlight on its bottom edge), a
// slim groove in it, and a cap with a line across it. Behaves as
// VolumeFader (follows the daemon, double-click for 0 dB).
VolumeFader {
    id: fader

    // Where a level sits on the groove (the centre of the cap there).
    function yFor(db) {
        const travel = availableHeight - Style.faderCapHeight;
        return topPadding + Style.faderCapHeight / 2 + (1 - (db - from) / (to - from)) * travel;
    }

    implicitWidth: Style.faderScaleWidth + Style.faderCapWidth
    padding: 0

    background: Item {
        // The scale: a tick and a number at the usual console marks.
        Repeater {
            model: [12, 6, 0, -6, -12, -24, -40, -60]

            delegate: Item {
                required property var modelData
                y: fader.yFor(modelData)
                width: Style.faderScaleWidth

                QQC2.Label {
                    anchors.right: tick.left
                    anchors.rightMargin: 2
                    anchors.verticalCenter: parent.top
                    text: parent.modelData <= -60 ? "∞" : parent.modelData > 0 ? "+" + parent.modelData
                                                                               : parent.modelData
                    font.pixelSize: Math.round(Kirigami.Theme.smallFont.pixelSize * 0.85)
                    font.bold: parent.modelData === 0
                    color: Style.scaleText
                }
                Rectangle {
                    id: tick
                    anchors.right: parent.right
                    width: parent.modelData === 0 ? 6 : 4
                    height: 1
                    color: Style.scaleTick
                }
            }
        }

        // The panel's lit edge around the slot (1 px, outside its rim).
        Rectangle {
            x: slot.x - 1
            y: slot.y - 1
            width: slot.width + 2
            height: slot.height + 2
            radius: slot.radius + 1
            color: "transparent"
            border.color: Style.slotEdgeLight
            border.width: 1
        }

        // The slot, inset into the panel.
        Rectangle {
            id: slot
            x: Style.faderScaleWidth - Style.faderSlotMargin
            y: fader.topPadding
            width: Style.faderCapWidth + 2 * Style.faderSlotMargin
            height: fader.availableHeight
            radius: 3
            // Darker at the sides, as if the light falls into a channel.
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Style.slotShadeEdge }
                GradientStop { position: 0.2; color: Style.slotShadeMid }
                GradientStop { position: 0.8; color: Style.slotShadeMid }
                GradientStop { position: 1.0; color: Style.slotShadeEdge }
            }
            border.color: Style.slotRim
            border.width: 1

            // The upper edge casts a shadow into the slot...
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                height: Math.round(Kirigami.Units.smallSpacing * 1.5)
                radius: 2
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Style.slotShadow }
                    GradientStop { position: 1.0; color: "transparent" }
                }
            }
            // ...and the lower edge catches the light.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 1
                height: 1
                color: Style.slotHighlight
            }
        }

        // The groove.
        Rectangle {
            x: Style.faderScaleWidth + (Style.faderCapWidth - width) / 2
            y: fader.topPadding + Style.faderCapHeight / 2
            width: Style.faderGrooveWidth
            height: fader.availableHeight - Style.faderCapHeight
            radius: width / 2
            color: Style.faderGroove
        }
    }

    handle: Rectangle {
        x: Style.faderScaleWidth
        y: fader.topPadding + fader.visualPosition * (fader.availableHeight - height)
        width: Style.faderCapWidth
        height: Style.faderCapHeight
        radius: 3
        color: fader.pressed ? Qt.darker(Kirigami.Theme.backgroundColor, 1.08) : Kirigami.Theme.backgroundColor
        border.color: fader.hovered || fader.pressed ? Kirigami.Theme.highlightColor : Style.faderCapBorder
        border.width: 1

        // The line across the cap marks the level.
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 6
            height: 2
            color: Style.faderCapLine
        }
    }
}

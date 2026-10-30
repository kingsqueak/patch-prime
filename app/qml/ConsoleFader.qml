// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's fader, drawn like a console's: a dB scale on the left, a slim
// groove, and a cap with a line across it. Behaves as VolumeFader (follows
// the daemon, double-click for 0 dB).
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

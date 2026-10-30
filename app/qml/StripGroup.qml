// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A titled row of channel strips (the inputs, the mix buses or the
// outputs), the title marked in the stage's color like its strips.
ColumnLayout {
    id: group

    property string title
    property string stage // "input", "bus" or "output"
    property alias strips: repeater.model
    readonly property int count: repeater.count

    spacing: Style.gap

    RowLayout {
        spacing: Style.gap

        Rectangle {
            implicitWidth: Kirigami.Units.gridUnit
            implicitHeight: Style.stageBarHeight
            radius: height / 2
            color: Style.stageColor(group.stage)
        }

        Kirigami.Heading {
            level: 5
            text: group.title.toUpperCase()
            font.letterSpacing: 1
            color: Style.dimText
        }
    }

    RowLayout {
        Layout.fillHeight: true
        spacing: Style.stripGap

        Repeater {
            id: repeater

            delegate: ChannelStrip {
                Layout.fillHeight: true
            }
        }
    }
}

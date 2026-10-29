// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A titled row of channel strips (the inputs, or the outputs).
ColumnLayout {
    id: group

    property string title
    property alias strips: repeater.model
    readonly property int count: repeater.count

    spacing: Style.gap

    Kirigami.Heading {
        level: 4
        text: group.title
        color: Style.dimText
    }

    RowLayout {
        Layout.fillHeight: true
        spacing: Style.gap

        Repeater {
            id: repeater

            delegate: ChannelStrip {
                Layout.fillHeight: true
            }
        }
    }
}

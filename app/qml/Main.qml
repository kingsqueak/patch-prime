// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    title: "Patch Prime"
    width: Kirigami.Units.gridUnit * 50
    height: Kirigami.Units.gridUnit * 32

    pageStack.initialPage: Kirigami.Page {
        title: "Mixer"

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            icon.name: "audio-card"
            text: "Patch Prime"
            explanation: "The device matrix and channel strips arrive in Phase 1. "
                         + "For now, run patchprime-probe --summary to list the devices PipeWire sees."
        }
    }
}

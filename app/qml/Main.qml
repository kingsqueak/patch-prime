// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    // "devices" opens the Devices page first (set from PATCHPRIME_PAGE).
    property string startPage

    title: "Patch Prime"
    width: Kirigami.Units.gridUnit * 52
    height: Kirigami.Units.gridUnit * 34

    pageStack.initialPage: MixerPage {
        onOpenDevices: root.pageStack.push(devicesPage)
    }

    Component {
        id: devicesPage
        DevicesPage {}
    }

    Component.onCompleted: {
        if (startPage === "devices")
            pageStack.push(devicesPage);
    }
}

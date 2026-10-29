// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    // "devices", "routing" or "apps" opens that page first (set from PATCHPRIME_PAGE).
    property string startPage

    title: "Patch Prime"
    width: Kirigami.Units.gridUnit * 52
    height: Kirigami.Units.gridUnit * 34

    pageStack.initialPage: MixerPage {
        onOpenDevices: root.pageStack.push(devicesPage)
        onOpenRouting: root.pageStack.push(routingPage)
        onOpenApps: root.pageStack.push(appsPage)
    }

    Component {
        id: appsPage
        AppsPage {}
    }

    Component {
        id: devicesPage
        DevicesPage {}
    }

    Component {
        id: routingPage
        RoutingPage {}
    }

    Component.onCompleted: {
        if (startPage === "devices")
            pageStack.push(devicesPage);
        else if (startPage === "routing")
            pageStack.push(routingPage);
        else if (startPage === "apps")
            pageStack.push(appsPage);
    }
}

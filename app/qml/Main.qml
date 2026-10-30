// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    // "devices", "routing" or "apps" opens that page first (set from PATCHPRIME_PAGE).
    property string startPage
    // A strip label whose effects panel opens at start (set from PATCHPRIME_EFFECTS).
    property string openEffects

    title: "Patch Prime"
    width: Kirigami.Units.gridUnit * 52
    height: Kirigami.Units.gridUnit * 34

    // One page at a time, full width, with a back button. Side-by-side
    // columns put the wide mixer under the page next to it.
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn

    // Opens a page over the mixer, replacing any page already open.
    function open(page) {
        pageStack.pop(pageStack.initialPage);
        pageStack.push(page);
    }

    pageStack.initialPage: MixerPage {
        onOpenDevices: root.open(devicesPage)
        onOpenRouting: root.open(routingPage)
        onOpenApps: root.open(appsPage)
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
            open(devicesPage);
        else if (startPage === "routing")
            open(routingPage);
        else if (startPage === "apps")
            open(appsPage);
    }
}

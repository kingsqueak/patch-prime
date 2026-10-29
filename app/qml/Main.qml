// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

Kirigami.ApplicationWindow {
    id: root

    title: "Patch Prime"
    width: Kirigami.Units.gridUnit * 46
    height: Kirigami.Units.gridUnit * 34

    pageStack.initialPage: DevicesPage {}

    Connections {
        target: Devices
        function onReattached(label, oldNodeName, newNodeName) {
            root.showPassiveNotification(qsTr("Recognised “%1” under its new name").arg(label));
        }
    }
}

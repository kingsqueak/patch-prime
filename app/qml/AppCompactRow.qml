// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kingsqueak.patchprime.engine

// A running app in the collapsed Apps column: a MiniRow with its icon, and
// the same controls as its AppStrip, looked up by key the same way.
MiniRow {
    id: row

    required property string appKey
    readonly property var app: Devices.apps.find(a => a.key === appKey) || ({})
    readonly property bool known: app.volumeDb !== undefined && app.volumeDb !== null
    readonly property var levels: (Devices.appLevels[appKey] || {}).levels || []

    label: app.name || appKey
    iconName: (app.icon || "").length > 0 ? app.icon : "application-x-executable"
    muted: app.muted === true
    volumeDb: known ? app.volumeDb : 0
    level: levels.length > 0 ? Math.max(...levels) : 0
    usable: Devices.daemonRunning && known
    onRequestedVolume: db => Devices.setAppVolume(appKey, db)
    onRequestedMute: on => Devices.setAppMuted(appKey, on)

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: rowMenu.popup()
    }

    QQC2.Menu {
        id: rowMenu

        QQC2.MenuItem {
            text: qsTr("Hide from the Mixer")
            icon.name: "view-hidden"
            onTriggered: Devices.setAppHidden(row.appKey, true)
        }
    }
}

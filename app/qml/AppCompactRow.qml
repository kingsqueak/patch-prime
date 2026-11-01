// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A running app in the collapsed Apps column: icon, name and mute on a
// line, then a slim meter and a level slider. The same controls as its
// AppStrip, looked up by key the same way.
ColumnLayout {
    id: row

    required property string appKey
    readonly property var app: Devices.apps.find(a => a.key === appKey) || ({})
    readonly property string label: app.name || appKey
    readonly property bool known: app.volumeDb !== undefined && app.volumeDb !== null
    readonly property bool muted: app.muted === true
    readonly property var levels: (Devices.appLevels[appKey] || {}).levels || []
    readonly property real level: levels.length > 0 ? Math.max(...levels) : 0

    spacing: 2

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

    RowLayout {
        Layout.fillWidth: true
        spacing: Style.gap

        Kirigami.Icon {
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            source: (row.app.icon || "").length > 0 ? row.app.icon : "application-x-executable"
            fallback: "application-x-executable"
            opacity: row.muted ? 0.4 : 1
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: row.label
            elide: Text.ElideRight
            font: Kirigami.Theme.smallFont
            QQC2.ToolTip.text: row.label
            QQC2.ToolTip.visible: truncated && nameHover.hovered
            HoverHandler { id: nameHover }
        }

        StripToggle {
            implicitWidth: Style.controlHeight * 0.8
            implicitHeight: Style.controlHeight * 0.8
            letter: qsTr("M", "mute button")
            onColor: Style.muteOn
            text: qsTr("Mute %1").arg(row.label)
            on: row.muted
            enabled: Devices.daemonRunning && row.known
            onRequested: on => Devices.setAppMuted(row.appKey, on)
        }
    }

    // The meter: the louder channel, colored like the strips' meters.
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: Style.meterWidth
        radius: Style.meterRadius
        color: Style.meterBackground

        Rectangle {
            height: parent.height
            width: parent.width * Math.min(1, row.level)
            radius: parent.radius
            opacity: row.muted ? 0.35 : 1
            color: row.level >= Style.meterClip ? Style.meterClipColor
                 : row.level >= Style.meterWarn ? Style.meterWarnColor : Style.meterLow
        }
    }

    VolumeFader {
        Layout.fillWidth: true
        orientation: Qt.Horizontal
        volumeDb: row.known ? row.app.volumeDb : 0
        enabled: Devices.daemonRunning && row.known
        accessibleName: qsTr("Level for %1").arg(row.label)
        onRequested: db => Devices.setAppVolume(row.appKey, db)
        QQC2.ToolTip.text: qsTr("%1 dB (double-click for 0 dB)").arg((row.known ? row.app.volumeDb : 0).toFixed(1))
    }
}

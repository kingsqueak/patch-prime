// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// One level in a compact list (the mini window, the collapsed Apps): the name and mute on a line, then a slim
// meter and a level slider. `requestedVolume` and `requestedMute` ask for a
// change; the owner passes them on, and the values come back from the daemon.
ColumnLayout {
    id: row

    property string label
    property bool muted: false
    property real volumeDb: 0
    property real level: 0 // the louder channel, 0..1 of the meter
    property bool usable: true
    property string iconName // an app's, in front of its name

    signal requestedVolume(real db)
    signal requestedMute(bool on)

    spacing: 2

    RowLayout {
        Layout.fillWidth: true
        spacing: Style.gap

        Kirigami.Icon {
            visible: row.iconName.length > 0
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            source: row.iconName
            fallback: "application-x-executable"
            opacity: row.muted ? 0.4 : 1
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: row.label
            elide: Text.ElideRight
            font: Kirigami.Theme.smallFont
            opacity: row.muted ? 0.6 : 1
            QQC2.ToolTip.text: row.label
            QQC2.ToolTip.visible: truncated && nameHover.hovered
            HoverHandler { id: nameHover }
        }

        StripToggle {
            implicitWidth: Style.controlHeight * 0.8
            implicitHeight: Style.controlHeight * 0.8
            letter: qsTr("M", "mute button")
            onColor: Style.muteOn
            text: qsTr("Mute")
            accessibleName: qsTr("Mute %1").arg(row.label)
            on: row.muted
            enabled: row.usable
            onRequested: on => row.requestedMute(on)
        }
    }

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
        volumeDb: row.volumeDb
        enabled: row.usable
        accessibleName: qsTr("Level for %1").arg(row.label)
        onRequested: db => row.requestedVolume(db)
        QQC2.ToolTip.text: qsTr("%1 dB (double-click for 0 dB)").arg(row.volumeDb.toFixed(1))
    }
}

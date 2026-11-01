// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Scan Plugins: looks for plugins installed since the last scan (the engine
// doesn't scan at every start: with many plugins it takes a while). While it
// runs, how far it is: files read of all, and plugins found so far.
RowLayout {
    id: scan

    readonly property int done: Devices.pluginsScan.done || 0
    readonly property int total: Devices.pluginsScan.total || 0
    readonly property int found: Devices.pluginsScan.found || 0

    spacing: Kirigami.Units.smallSpacing

    QQC2.Button {
        visible: !Devices.pluginsScanning
        icon.name: "view-refresh"
        text: qsTr("Scan Plugins")
        enabled: Devices.daemonRunning
        onClicked: Devices.rescanPlugins()
        QQC2.ToolTip.text: Devices.pluginsLastScan.getTime()
                           ? qsTr("Look for plugins installed since the last scan (%1). %2 plugins known.")
                             .arg(Devices.pluginsLastScan.toLocaleString(Qt.locale(), Locale.ShortFormat))
                             .arg(Devices.plugins.length)
                           : qsTr("Look for installed plugins")
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    QQC2.ProgressBar {
        visible: Devices.pluginsScanning
        Layout.preferredWidth: Kirigami.Units.gridUnit * 8
        from: 0
        to: Math.max(scan.total, 1)
        value: scan.done
        indeterminate: scan.total === 0
        Accessible.name: qsTr("Scanning plugins")
    }
    QQC2.Label {
        visible: Devices.pluginsScanning
        font: Kirigami.Theme.smallFont
        text: scan.total > 0
              ? qsTr("Scanning: %1 of %2 files, %3 plugins").arg(scan.done).arg(scan.total).arg(scan.found)
              : qsTr("Scanning plugins…")
    }
}

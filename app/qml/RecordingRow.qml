// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// One recording in a list: its name and details, play and move to trash.
// Double-click plays it too. Compact: smaller, without the icon.
QQC2.ItemDelegate {
    id: row

    required property var modelData
    required property RecordingsState rec
    property bool compact: false
    readonly property bool current: modelData.path === rec.playing

    highlighted: current
    onDoubleClicked: rec.play(modelData.path)
    leftPadding: compact ? Kirigami.Units.smallSpacing : Kirigami.Units.largeSpacing
    rightPadding: leftPadding
    topPadding: compact ? 2 : Kirigami.Units.smallSpacing
    bottomPadding: topPadding

    component Tool: QQC2.ToolButton {
        display: QQC2.AbstractButton.IconOnly
        Layout.preferredWidth: row.compact ? Style.controlHeight : -1
        Layout.preferredHeight: row.compact ? Style.controlHeight : -1
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    contentItem: RowLayout {
        spacing: row.compact ? Kirigami.Units.smallSpacing : Kirigami.Units.largeSpacing

        Kirigami.Icon {
            visible: !row.compact
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
            source: row.current ? "media-playback-start" : "audio-x-generic"
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            QQC2.Label {
                Layout.fillWidth: true
                text: row.modelData.name
                elide: row.compact ? Text.ElideMiddle : Text.ElideRight
                font.pointSize: row.compact ? Kirigami.Theme.smallFont.pointSize
                                            : Kirigami.Theme.defaultFont.pointSize
                QQC2.ToolTip.text: row.modelData.name
                QQC2.ToolTip.visible: row.compact && truncated && row.hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.Label {
                Layout.fillWidth: true
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                elide: Text.ElideRight
                text: row.rec.details(row.modelData)
            }
        }
        Tool {
            icon.name: "media-playback-start"
            text: qsTr("Play")
            enabled: row.rec.target.length > 0
            onClicked: row.rec.play(row.modelData.path)
            QQC2.ToolTip.text: qsTr("Play into %1").arg(row.rec.targetInfo.name || "")
        }
        Tool {
            icon.name: "user-trash"
            text: qsTr("Move to Trash")
            enabled: !row.rec.isBusy(row.modelData.path)
            onClicked: row.rec.trash(row.modelData.path)
            QQC2.ToolTip.text: text
        }
    }
}

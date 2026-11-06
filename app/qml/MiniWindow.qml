// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The mixer collapsed to one strip's width: every level in a stack, each a
// name, a mute and a slider, grouped by stage in the mixer's order, apps
// after the inputs. A window of its own (the full window hides meanwhile),
// so it keeps its own height, and can stay on top. Scrolls when it's taller
// than the window.
QQC2.ApplicationWindow {
    id: mini

    signal expand()

    readonly property real rowWidth: Style.stripWidth
    // Room for the scroll bar, which then never covers the rows.
    readonly property real fixedWidth: rowWidth + Style.gap * 2 + Kirigami.Units.gridUnit

    // The apps shown, by key; only reassigned when that list changes, so
    // the rows aren't rebuilt on every level change.
    property var appKeys: []
    function refreshApps() {
        const wanted = Devices.apps
            .filter(a => a.direction === "playback" && a.streams > 0 && !a.hidden)
            .map(a => a.key);
        if (JSON.stringify(wanted) !== JSON.stringify(appKeys))
            appKeys = wanted;
    }

    // Until the user sizes it, it's as tall as its rows, as far as that fits.
    readonly property int fitHeight: Math.min(Screen.desktopAvailableHeight * 0.85,
                                              stack.implicitHeight + toolBar.implicitHeight + Style.gap * 2)
    function fit() {
        height = fitHeight;
    }
    onFitHeightChanged: {
        if (saved.height === 0)
            fit();
    }
    // A height of its own only if the user gave it one.
    function rememberHeight() {
        saved.height = Math.abs(height - fitHeight) > 1 ? height : 0;
    }
    onVisibleChanged: {
        if (!visible)
            rememberHeight();
    }
    onClosing: rememberHeight()

    function open() {
        if (saved.height > 0)
            height = Math.min(saved.height, Screen.desktopAvailableHeight);
        else
            fit();
        show();
        raise();
        requestActivate();
    }

    title: qsTr("Mini Mixer")
    flags: saved.onTop ? Qt.Window | Qt.WindowStaysOnTopHint : Qt.Window
    width: fixedWidth
    minimumWidth: fixedWidth
    maximumWidth: fixedWidth
    minimumHeight: Kirigami.Units.gridUnit * 8
    color: Kirigami.Theme.backgroundColor
    Component.onCompleted: refreshApps()

    Settings {
        id: saved
        category: "MiniWindow"
        property int height: 0
        property bool onTop: false
    }

    Connections {
        target: Devices
        function onAppsChanged() { mini.refreshApps(); }
    }

    StripFilter {
        id: inputs
        sourceModel: Devices
        stages: ["input"]
    }
    StripFilter {
        id: buses
        sourceModel: Devices
        stages: ["bus"]
    }
    StripFilter {
        id: outputs
        sourceModel: Devices
        stages: ["output"]
    }

    // A stage's rows on a card, its color along the top, as on the strips.
    component Stage: QQC2.Control {
        id: card

        property color stageColor
        property string name
        default property alias rows: column.data

        Layout.fillWidth: true
        padding: Style.stripPadding
        topPadding: Style.stripPadding + Style.stageBarHeight
        Accessible.role: Accessible.Grouping
        Accessible.name: name

        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false

        background: Rectangle {
            color: Kirigami.Theme.backgroundColor
            border.color: Style.stripBorder
            border.width: 1
            radius: Style.stripRadius

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                height: Style.stageBarHeight
                radius: Style.stripRadius
                color: card.stageColor
            }
        }

        contentItem: ColumnLayout {
            id: column
            spacing: Kirigami.Units.smallSpacing
        }
    }

    component StripRows: Repeater {
        delegate: MiniRow {
            required property var model

            Layout.fillWidth: true
            label: model.label
            muted: model.muted
            volumeDb: model.volumeDb
            level: (model.levels || []).length > 0 ? Math.max(...model.levels) : 0
            usable: Devices.daemonRunning
            onRequestedVolume: db => Devices.setVolume(model.key, db)
            onRequestedMute: on => Devices.setMuted(model.key, on)
        }
    }

    header: QQC2.ToolBar {
        id: toolBar

        RowLayout {
            anchors.fill: parent

            QQC2.ToolButton {
                icon.name: "view-media-equalizer"
                text: qsTr("Full Mixer")
                display: QQC2.AbstractButton.IconOnly
                onClicked: mini.expand()
                QQC2.ToolTip.text: qsTr("Back to the full mixer (Ctrl+M)")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }

            Item { Layout.fillWidth: true }

            QQC2.ToolButton {
                icon.name: "window-pin"
                text: qsTr("Keep on Top")
                display: QQC2.AbstractButton.IconOnly
                checkable: true
                checked: saved.onTop
                onToggled: saved.onTop = checked
                QQC2.ToolTip.text: qsTr("Keep this window above the others")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }

    EngineMessage {
        id: message
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }

    QQC2.ScrollView {
        id: scroller

        anchors.fill: parent
        anchors.topMargin: message.visible ? message.height : 0
        contentWidth: availableWidth
        QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

        ColumnLayout {
            id: stack

            x: Style.gap
            y: Style.gap
            width: mini.rowWidth
            spacing: Style.gap

            Stage {
                visible: inputs.count > 0
                name: qsTr("Inputs")
                stageColor: Style.inputColor
                StripRows { model: inputs }
            }

            Stage {
                visible: mini.appKeys.length > 0
                name: qsTr("Apps")
                stageColor: Style.appColor

                Repeater {
                    model: mini.appKeys

                    delegate: MiniRow {
                        required property string modelData
                        readonly property var app: Devices.apps.find(a => a.key === modelData) || ({})
                        readonly property bool known: app.volumeDb !== undefined && app.volumeDb !== null
                        readonly property var levels: (Devices.appLevels[modelData] || {}).levels || []

                        Layout.fillWidth: true
                        label: app.name || modelData
                        muted: app.muted === true
                        volumeDb: known ? app.volumeDb : 0
                        level: levels.length > 0 ? Math.max(...levels) : 0
                        usable: Devices.daemonRunning && known
                        onRequestedVolume: db => Devices.setAppVolume(modelData, db)
                        onRequestedMute: on => Devices.setAppMuted(modelData, on)
                    }
                }
            }

            Stage {
                visible: buses.count > 0
                name: qsTr("Mix Buses")
                stageColor: Style.busColor
                StripRows { model: buses }
            }

            Stage {
                visible: outputs.count > 0
                name: qsTr("Outputs")
                stageColor: Style.outputColor
                StripRows { model: outputs }
            }

            Item { implicitHeight: Style.gap }
        }
    }
}

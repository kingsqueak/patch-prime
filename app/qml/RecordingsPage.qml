// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Recordings: the files the record buttons made, the player that plays them
// into a strip, and the recording format.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Recordings")

    header: EngineMessage {}

    property var files: []
    readonly property string playing: Devices.player.path || ""
    readonly property bool paused: Devices.player.paused === true
    readonly property real duration: Devices.player.duration || 0

    // Where the player can play: mix buses, routed inputs and hardware outputs.
    readonly property var targets: {
        const list = [];
        for (let row = 0; row < strips.count; ++row) {
            const s = strips.get(row);
            if (s.kind === "virtual-sink" || s.kind === "virtual-input"
                    || (s.kind === "hardware" && s.direction === "output"))
                list.push({ key: s.key, label: s.label });
        }
        return list;
    }
    readonly property string target: {
        if (targets.some(t => t.key === Devices.playerTarget))
            return Devices.playerTarget;
        return targets.length > 0 ? targets[0].key : "";
    }

    function refresh() {
        files = Devices.daemonRunning ? Devices.recordings() : [];
    }
    function play(path) {
        if (target !== Devices.playerTarget)
            Devices.setPlayerTarget(target);
        Devices.playRecording(path);
    }
    function size(bytes) {
        if (bytes >= 1024 * 1024 * 1024)
            return qsTr("%1 GB").arg((bytes / (1024 * 1024 * 1024)).toFixed(1));
        if (bytes >= 1024 * 1024)
            return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1));
        return qsTr("%1 kB").arg(Math.max(1, Math.round(bytes / 1024)));
    }

    Component.onCompleted: refresh()
    Connections {
        target: Devices
        function onRecordingsRevisionChanged() { page.refresh(); }
        function onDaemonRunningChanged() { page.refresh(); }
    }

    StripFilter {
        id: strips
        sourceModel: Devices
        stages: ["input", "bus", "output"]
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: Devices.recordingError.length > 0
            type: Kirigami.MessageType.Error
            text: qsTr("A recording stopped: %1").arg(Devices.recordingError)
        }

        // --- the player
        Kirigami.AbstractCard {
            Layout.fillWidth: true

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true

                    QQC2.ToolButton {
                        icon.name: page.playing.length > 0 && !page.paused
                                   ? "media-playback-pause" : "media-playback-start"
                        text: page.playing.length > 0 && !page.paused ? qsTr("Pause") : qsTr("Play")
                        display: QQC2.AbstractButton.IconOnly
                        enabled: page.playing.length > 0 || page.files.length > 0
                        onClicked: {
                            if (page.playing.length > 0)
                                Devices.pausePlayback(!page.paused);
                            else
                                page.play(page.files[0].path);
                        }
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                    QQC2.ToolButton {
                        icon.name: "media-playback-stop"
                        text: qsTr("Stop")
                        display: QQC2.AbstractButton.IconOnly
                        enabled: page.playing.length > 0
                        onClicked: Devices.stopPlayback()
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        text: page.playing.length > 0 ? page.playing.split("/").pop()
                                                      : qsTr("Nothing playing")
                        opacity: page.playing.length > 0 ? 1 : 0.6
                    }
                    QQC2.Label {
                        text: qsTr("Play into")
                    }
                    QQC2.ComboBox {
                        id: targetBox
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 11
                        model: page.targets
                        textRole: "label"
                        valueRole: "key"
                        enabled: page.targets.length > 0
                        currentIndex: page.targets.findIndex(t => t.key === page.target)
                        onActivated: Devices.setPlayerTarget(currentValue)
                        Accessible.name: qsTr("Play into")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    QQC2.Slider {
                        id: position
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(page.duration, 0.001)
                        enabled: page.playing.length > 0
                        // Follows the player, except while dragged; a
                        // drag seeks when let go (see VolumeFader.qml).
                        onPressedChanged: {
                            if (!pressed)
                                Devices.seekPlayback(value);
                        }
                        Accessible.name: qsTr("Position")
                    }
                    Connections {
                        target: Devices
                        function onRecordingTimesChanged() {
                            if (!position.pressed)
                                position.value = Devices.playerPosition;
                        }
                    }
                    QQC2.Label {
                        font.family: "monospace"
                        text: qsTr("%1 / %2").arg(Style.clock(position.pressed ? position.value
                                                                                : Devices.playerPosition))
                                             .arg(Style.clock(page.duration))
                    }
                }
            }
        }

        // --- settings
        RowLayout {
            Layout.fillWidth: true

            QQC2.Label {
                text: qsTr("Record as")
            }
            QQC2.ComboBox {
                model: [
                    { value: "flac", text: qsTr("FLAC (lossless)") },
                    { value: "wav", text: qsTr("WAV (uncompressed)") },
                    { value: "mp3", text: qsTr("MP3 (small, up to 48 kHz)") }
                ].filter(f => Devices.recordingFormats.includes(f.value))
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, model.findIndex(m => m.value === Devices.recordingFormat))
                onActivated: Devices.setRecordingFormat(currentValue)
                Accessible.name: qsTr("Recording format")
            }
            Item { Layout.fillWidth: true }
            QQC2.Button {
                icon.name: "folder-open"
                text: qsTr("Open Folder")
                enabled: Devices.recordingFolder.length > 0
                onClicked: Qt.openUrlExternally("file://" + Devices.recordingFolder)
            }
        }

        // --- the files
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 2
            visible: page.files.length === 0
            icon.name: "media-record"
            text: qsTr("No recordings yet")
            explanation: qsTr("Press the record button on a mix bus or on Desktop in the mixer. Recordings go to %1.")
                         .arg(Devices.recordingFolder)
        }

        Repeater {
            model: page.files

            delegate: QQC2.ItemDelegate {
                id: row

                required property var modelData
                readonly property bool current: modelData.path === page.playing

                Layout.fillWidth: true
                highlighted: current
                onDoubleClicked: page.play(modelData.path)

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
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
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            font: Kirigami.Theme.smallFont
                            opacity: 0.7
                            text: [row.modelData.seconds > 0 ? Style.clock(row.modelData.seconds) : "",
                                   page.size(row.modelData.bytes),
                                   new Date(row.modelData.modified).toLocaleString(Qt.locale(), Locale.ShortFormat)]
                                  .filter(s => s.length > 0).join(" · ")
                        }
                    }
                    QQC2.ToolButton {
                        icon.name: "media-playback-start"
                        text: qsTr("Play")
                        display: QQC2.AbstractButton.IconOnly
                        enabled: page.target.length > 0
                        onClicked: page.play(row.modelData.path)
                        QQC2.ToolTip.text: qsTr("Play into %1").arg(targetBox.currentText)
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                    QQC2.ToolButton {
                        icon.name: "user-trash"
                        text: qsTr("Move to Trash")
                        display: QQC2.AbstractButton.IconOnly
                        enabled: !row.current && !Object.values(Devices.recording).includes(row.modelData.path)
                        onClicked: {
                            if (Devices.trashFile(row.modelData.path))
                                page.refresh();
                        }
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }
            }
        }
    }
}

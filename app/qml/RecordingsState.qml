// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kingsqueak.patchprime.engine

// What the Recordings page and the mixer's Recordings panel share: the
// recordings folder's files (refreshed as they change), where the player
// can play, and the player's state.
Item {
    id: state

    property var files: []
    readonly property string playing: Devices.player.path || ""
    readonly property bool paused: Devices.player.paused === true
    readonly property bool looping: Devices.player.loop === true
    readonly property real duration: Devices.player.duration || 0

    // Where the player can play: hardware inputs (in place of the device),
    // mix buses, routed inputs and hardware outputs, in console order.
    readonly property var targets: {
        const list = [];
        for (let row = 0; row < strips.count; ++row) {
            const s = strips.get(row);
            if (s.kind === "hardware" && s.direction === "input")
                list.push({ key: s.key, label: qsTr("%1 (instead of the device)").arg(s.label),
                            name: s.label, input: true });
            else if (s.kind === "virtual-sink" || s.kind === "virtual-input" || s.kind === "hardware")
                list.push({ key: s.key, label: s.label, name: s.label, input: false });
        }
        return list;
    }
    readonly property string target: {
        if (targets.some(t => t.key === Devices.playerTarget))
            return Devices.playerTarget;
        return targets.length > 0 ? targets[0].key : "";
    }
    readonly property var targetInfo: targets.find(t => t.key === target) || ({})
    readonly property var formats: [
        { value: "flac", text: qsTr("FLAC (lossless)"), short: "FLAC" },
        { value: "wav", text: qsTr("WAV (uncompressed)"), short: "WAV" },
        { value: "mp3", text: qsTr("MP3 (small, up to 48 kHz)"), short: "MP3" }
    ].filter(f => Devices.recordingFormats.includes(f.value))

    function refresh() {
        files = Devices.daemonRunning ? Devices.recordings() : [];
    }
    function play(path) {
        if (target !== Devices.playerTarget)
            Devices.setPlayerTarget(target);
        Devices.playRecording(path);
    }
    // Play, pause or resume: the newest file when nothing's loaded.
    function playPause() {
        if (playing.length > 0)
            Devices.pausePlayback(!paused);
        else if (files.length > 0)
            play(files[0].path);
    }
    function trash(path) {
        if (Devices.trashFile(path))
            refresh();
    }
    function isBusy(path) { // playing or being recorded: not to be trashed
        return path === playing || Object.values(Devices.recording).includes(path);
    }
    function size(bytes) {
        if (bytes >= 1024 * 1024 * 1024)
            return qsTr("%1 GB").arg((bytes / (1024 * 1024 * 1024)).toFixed(1));
        if (bytes >= 1024 * 1024)
            return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1));
        return qsTr("%1 kB").arg(Math.max(1, Math.round(bytes / 1024)));
    }
    function details(file) { // "0:12 · 1.2 MB · date"
        return [file.seconds > 0 ? Style.clock(file.seconds) : "", size(file.bytes),
                new Date(file.modified).toLocaleString(Qt.locale(), Locale.ShortFormat)]
               .filter(s => s.length > 0).join(" · ");
    }

    Component.onCompleted: refresh()
    Connections {
        target: Devices
        function onRecordingsRevisionChanged() { state.refresh(); }
        function onDaemonRunningChanged() { state.refresh(); }
    }

    StripFilter {
        id: strips
        sourceModel: Devices
        stages: ["input", "bus", "output"]
    }
}

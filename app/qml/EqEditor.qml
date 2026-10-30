// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The 6-band parametric EQ: its response curve with a handle per band
// (drag for frequency and gain, scroll for Q, double-click to flatten), and
// a table of the bands with typed values.
//
// While a handle is dragged the curve follows the pointer at once (`live`
// overrides the daemon's values) and the daemon is told as it goes; on
// release the daemon's values take over again.
ColumnLayout {
    id: editor

    property string key       // the strip
    property int index        // the effect's place in the strip's chain
    property var params: ({}) // the daemon's values for this effect

    property var live: ({})   // values being dragged, by name

    readonly property int bandCount: 6
    readonly property real minFreq: 20
    readonly property real maxFreq: 20000
    readonly property real rangeDb: 24
    readonly property var typeNames: [qsTr("Bell"), qsTr("Low shelf"), qsTr("High shelf"),
                                      qsTr("Low cut"), qsTr("High cut")]

    function param(name, fallback) {
        if (live[name] !== undefined)
            return live[name];
        const v = params[name];
        return v === undefined ? fallback : v;
    }
    function bandParam(band, field) {
        const defaults = {
            On: 1,
            Type: [1, 0, 0, 0, 0, 2][band - 1],
            Freq: [80, 250, 1000, 3000, 6000, 12000][band - 1],
            Gain: 0,
            Q: band === 1 || band === 6 ? 0.707 : 1
        };
        return param("b" + band + field, defaults[field]);
    }
    function set(name, value) {
        Devices.setEffectParam(key, index, name, value);
    }
    // Bells and shelves have a gain; cuts don't.
    function hasGain(band) {
        return bandParam(band, "Type") < 3;
    }
    function merged() {
        const all = Object.assign({}, params);
        for (const name in live)
            all[name] = live[name];
        return all;
    }

    spacing: Kirigami.Units.largeSpacing

    // --- the graph
    Rectangle {
        id: graph

        // Frequency <-> x on a log scale, dB <-> y.
        function xFor(freq) {
            return width * Math.log(freq / editor.minFreq) / Math.log(editor.maxFreq / editor.minFreq);
        }
        function freqAt(x) {
            return editor.minFreq * Math.pow(editor.maxFreq / editor.minFreq, Math.min(Math.max(x / width, 0), 1));
        }
        function yFor(db) {
            return height / 2 - db / editor.rangeDb * (height / 2);
        }
        function dbAt(y) {
            return Math.min(Math.max((height / 2 - y) / (height / 2) * editor.rangeDb, -editor.rangeDb), editor.rangeDb);
        }

        Layout.fillWidth: true
        Layout.preferredHeight: Style.graphHeight
        Layout.minimumWidth: Kirigami.Units.gridUnit * 30
        color: Style.graphBackground
        radius: Kirigami.Units.cornerRadius
        clip: true

        Canvas {
            id: canvas

            anchors.fill: parent

            // Repaint when anything that shapes the curve changes.
            property var shape: [editor.params, editor.live, Devices.sampleRate, width, height]
            onShapeChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const w = width;
                const h = height;
                // Grid: decades and octave-ish marks, and every 6 dB.
                ctx.strokeStyle = Style.graphGrid;
                ctx.fillStyle = Style.graphLabel;
                ctx.lineWidth = 1;
                ctx.font = Math.round(Kirigami.Theme.smallFont.pixelSize) + "px sans-serif";
                const marks = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
                for (const f of marks) {
                    const x = Math.round(graph.xFor(f)) + 0.5;
                    ctx.beginPath();
                    ctx.moveTo(x, 0);
                    ctx.lineTo(x, h);
                    ctx.stroke();
                    ctx.fillText(f >= 1000 ? (f / 1000) + "k" : f, x + 3, h - 4);
                }
                for (let db = -18; db <= 18; db += 6) {
                    const y = Math.round(graph.yFor(db)) + 0.5;
                    ctx.globalAlpha = db === 0 ? 1 : 0.6;
                    ctx.beginPath();
                    ctx.moveTo(0, y);
                    ctx.lineTo(w, y);
                    ctx.stroke();
                    if (db !== 0)
                        ctx.fillText((db > 0 ? "+" : "") + db, 3, y - 3);
                }
                ctx.globalAlpha = 1;

                // The response, one point every few pixels.
                const points = Math.max(2, Math.round(w / 3));
                const freqs = [];
                for (let i = 0; i < points; ++i)
                    freqs.push(graph.freqAt(i / (points - 1) * w));
                const curve = Devices.eqCurve(editor.merged(), freqs);
                ctx.beginPath();
                for (let i = 0; i < points; ++i) {
                    const x = i / (points - 1) * w;
                    const y = graph.yFor(Math.min(Math.max(curve[i], -editor.rangeDb * 1.5), editor.rangeDb * 1.5));
                    if (i === 0)
                        ctx.moveTo(x, y);
                    else
                        ctx.lineTo(x, y);
                }
                ctx.strokeStyle = Style.graphCurve;
                ctx.lineWidth = 2;
                ctx.stroke();
                // Fill towards 0 dB, faintly.
                ctx.lineTo(w, graph.yFor(0));
                ctx.lineTo(0, graph.yFor(0));
                ctx.closePath();
                ctx.globalAlpha = 0.12;
                ctx.fillStyle = Style.graphCurve;
                ctx.fill();
            }
        }

        // A handle per band.
        Repeater {
            model: editor.bandCount

            delegate: Rectangle {
                id: handle

                required property int index
                readonly property int band: index + 1
                readonly property bool on: editor.bandParam(band, "On") >= 0.5
                readonly property real freq: editor.bandParam(band, "Freq")
                readonly property real gain: editor.hasGain(band) ? editor.bandParam(band, "Gain") : 0

                width: Style.handleSize
                height: width
                radius: width / 2
                x: graph.xFor(freq) - width / 2
                y: graph.yFor(gain) - height / 2
                color: on ? Style.bandColors[index] : "transparent"
                border.color: Style.bandColors[index]
                border.width: 2
                opacity: on ? 1 : 0.6
                z: drag.active ? 2 : 1

                QQC2.Label {
                    anchors.centerIn: parent
                    text: handle.band
                    font: Kirigami.Theme.smallFont
                    color: handle.on ? "white" : Kirigami.Theme.textColor
                }

                DragHandler {
                    id: drag

                    target: null
                    // Where the pointer grabbed, relative to the handle's centre.
                    property point start
                    onActiveChanged: {
                        if (active) {
                            start = Qt.point(graph.xFor(handle.freq), graph.yFor(handle.gain));
                        } else {
                            editor.live = {}; // the daemon's values from here
                        }
                    }
                    onTranslationChanged: {
                        if (!active)
                            return;
                        const prefix = "b" + handle.band;
                        const freq = Math.round(graph.freqAt(start.x + translation.x));
                        const next = Object.assign({}, editor.live);
                        next[prefix + "Freq"] = freq;
                        editor.set(prefix + "Freq", freq);
                        if (editor.hasGain(handle.band)) {
                            const gain = Math.round(graph.dbAt(start.y + translation.y) * 10) / 10;
                            next[prefix + "Gain"] = gain;
                            editor.set(prefix + "Gain", gain);
                        }
                        editor.live = next;
                    }
                }

                WheelHandler {
                    // Scroll for Q: up is narrower.
                    onWheel: event => {
                        const q = editor.bandParam(handle.band, "Q");
                        const next = Math.min(Math.max(q * Math.pow(1.1, event.angleDelta.y / 120), 0.1), 10);
                        editor.set("b" + handle.band + "Q", Math.round(next * 100) / 100);
                    }
                }

                TapHandler {
                    onDoubleTapped: editor.set("b" + handle.band + "Gain", 0)
                }

                HoverHandler {
                    id: hover
                    cursorShape: Qt.SizeAllCursor
                }
                QQC2.ToolTip.visible: hover.hovered && !drag.active
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("Band %1: drag to move, scroll for width (Q), double-click for 0 dB").arg(band)
            }
        }
    }

    // --- the bands as a table: a column per band
    GridLayout {
        Layout.fillWidth: true
        columns: editor.bandCount + 1
        columnSpacing: Kirigami.Units.smallSpacing
        rowSpacing: Kirigami.Units.smallSpacing

        // Header row
        Item { implicitWidth: 1 }
        Repeater {
            model: editor.bandCount
            delegate: RowLayout {
                required property int index
                readonly property int band: index + 1
                Layout.fillWidth: true

                Rectangle {
                    implicitWidth: Kirigami.Units.smallSpacing * 2
                    implicitHeight: implicitWidth
                    radius: width / 2
                    color: Style.bandColors[parent.index]
                }
                QQC2.CheckBox {
                    text: qsTr("Band %1").arg(parent.band)
                    checked: editor.bandParam(parent.band, "On") >= 0.5
                    onToggled: editor.set("b" + parent.band + "On", checked ? 1 : 0)
                }
            }
        }

        QQC2.Label { text: qsTr("Type") }
        Repeater {
            model: editor.bandCount
            delegate: QQC2.ComboBox {
                required property int index
                Layout.fillWidth: true
                Layout.minimumWidth: Kirigami.Units.gridUnit * 5
                model: editor.typeNames
                currentIndex: editor.bandParam(index + 1, "Type")
                onActivated: i => editor.set("b" + (index + 1) + "Type", i)
                Accessible.name: qsTr("Band %1 type").arg(index + 1)
            }
        }

        QQC2.Label { text: qsTr("Frequency") }
        Repeater {
            model: editor.bandCount
            delegate: ParamField {
                required property int index
                Layout.fillWidth: true
                unit: "Hz"
                value: editor.bandParam(index + 1, "Freq")
                accessibleName: qsTr("Band %1 frequency").arg(index + 1)
                onRequested: v => editor.set("b" + (index + 1) + "Freq", v)
            }
        }

        QQC2.Label { text: qsTr("Gain") }
        Repeater {
            model: editor.bandCount
            delegate: ParamField {
                required property int index
                Layout.fillWidth: true
                unit: "dB"
                enabled: editor.hasGain(index + 1)
                value: editor.bandParam(index + 1, "Gain")
                accessibleName: qsTr("Band %1 gain").arg(index + 1)
                onRequested: v => editor.set("b" + (index + 1) + "Gain", v)
            }
        }

        QQC2.Label { text: qsTr("Q") }
        Repeater {
            model: editor.bandCount
            delegate: ParamField {
                required property int index
                Layout.fillWidth: true
                value: editor.bandParam(index + 1, "Q")
                accessibleName: qsTr("Band %1 Q").arg(index + 1)
                onRequested: v => editor.set("b" + (index + 1) + "Q", v)
            }
        }
    }
}

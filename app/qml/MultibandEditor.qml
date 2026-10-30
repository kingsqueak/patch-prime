// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The 4-band compressor: the bands as spans on a frequency scale, each
// showing its gain reduction, with the three crossovers as lines to drag;
// below, the crossovers and each band's settings as typed values.
//
// While a crossover is dragged it follows the pointer at once (`live`) and
// the daemon is told as it goes; on release the daemon's value takes over.
ColumnLayout {
    id: editor

    property string key
    property int index
    property var params: ({})

    property var live: ({})

    readonly property int bandCount: 4
    readonly property real minFreq: 20
    readonly property real maxFreq: 20000
    readonly property real minCrossover: 30
    readonly property real maxCrossover: 16000
    readonly property real crossoverGap: 1.25 // crossovers stay this far apart (ratio)
    readonly property real meterRangeDb: 24
    // Gain reduction per band in dB, from the engine.
    readonly property var reduction: Devices.effectMeters[key + "|" + index] || []

    function param(name, fallback) {
        if (live[name] !== undefined)
            return live[name];
        const v = params[name];
        return v === undefined ? fallback : v;
    }
    function crossover(n) { // 1-3
        return param("x" + n, [120, 1000, 6000][n - 1]);
    }
    // The band edges: 20 Hz, the three crossovers, 20 kHz.
    function edge(i) {
        return i === 0 ? minFreq : i === 4 ? maxFreq : crossover(i);
    }
    function bandParam(band, field) {
        const defaults = { On: 1, Solo: 0, Threshold: -20, Ratio: 2, Attack: 10, Release: 150, Makeup: 0 };
        return param("b" + band + field, defaults[field]);
    }
    function set(name, value) {
        Devices.setEffectParam(key, index, name, value);
    }
    function formatFreq(f) {
        return f >= 1000 ? (f / 1000).toFixed(f >= 10000 ? 0 : 1) + "k" : Math.round(f);
    }

    spacing: Kirigami.Units.largeSpacing

    // --- band spans
    Rectangle {
        id: spans

        function xFor(freq) {
            return width * Math.log(freq / editor.minFreq) / Math.log(editor.maxFreq / editor.minFreq);
        }
        function freqAt(x) {
            return editor.minFreq * Math.pow(editor.maxFreq / editor.minFreq, Math.min(Math.max(x / width, 0), 1));
        }

        Layout.fillWidth: true
        Layout.preferredHeight: Style.graphHeight * 0.6
        Layout.minimumWidth: Kirigami.Units.gridUnit * 30
        color: Style.graphBackground
        radius: Kirigami.Units.cornerRadius
        clip: true

        // Frequency marks.
        Repeater {
            model: [50, 100, 200, 500, 1000, 2000, 5000, 10000]
            delegate: Item {
                required property var modelData
                x: spans.xFor(modelData)
                height: spans.height
                Rectangle {
                    width: 1
                    height: parent.height
                    color: Style.graphGrid
                }
                QQC2.Label {
                    x: 3
                    anchors.bottom: parent.bottom
                    text: editor.formatFreq(parent.modelData)
                    font: Kirigami.Theme.smallFont
                    color: Style.graphLabel
                }
            }
        }

        // A span per band, with its gain reduction growing down from the top.
        Repeater {
            model: editor.bandCount
            delegate: Item {
                id: span

                required property int index
                readonly property int band: index + 1
                readonly property bool on: editor.bandParam(band, "On") >= 0.5
                readonly property bool solo: editor.bandParam(band, "Solo") >= 0.5
                readonly property real reduction: index < editor.reduction.length ? editor.reduction[index] : 0

                x: spans.xFor(editor.edge(index))
                width: spans.xFor(editor.edge(index + 1)) - x
                height: spans.height

                Rectangle {
                    anchors.fill: parent
                    color: Style.bandColors[span.index]
                    opacity: span.on ? 0.14 : 0.04
                }
                // Soloed: outlined at full strength.
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.color: Style.bandColors[span.index]
                    border.width: 2
                    visible: span.solo
                }
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width * 0.5, Kirigami.Units.gridUnit * 2)
                    height: parent.height * Math.min(span.reduction, editor.meterRangeDb) / editor.meterRangeDb
                    color: Style.bandColors[span.index]
                    opacity: 0.8
                    radius: 2
                }
                QQC2.Label {
                    anchors.centerIn: parent
                    text: qsTr("Band %1").arg(span.band)
                          + (span.on ? "\n" + (span.reduction > 0.05 ? "−" + span.reduction.toFixed(1) + " dB" : "0 dB")
                                     : "\n" + qsTr("off"))
                    horizontalAlignment: Text.AlignHCenter
                    font: Kirigami.Theme.smallFont
                    visible: span.width > Kirigami.Units.gridUnit * 3
                }
            }
        }

        // The crossovers: a line each, dragged sideways.
        Repeater {
            model: 3
            delegate: Item {
                id: line

                required property int index
                readonly property int n: index + 1
                readonly property real freq: editor.crossover(n)

                x: spans.xFor(freq) - width / 2
                width: Kirigami.Units.gridUnit
                height: spans.height
                z: 2

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: drag.active || hover.hovered ? 3 : 2
                    height: parent.height
                    color: Kirigami.Theme.textColor
                    opacity: 0.7
                }

                DragHandler {
                    id: drag

                    target: null
                    xAxis.enabled: true
                    yAxis.enabled: false
                    property real startX
                    onActiveChanged: {
                        if (active)
                            startX = spans.xFor(line.freq);
                        else
                            editor.live = {};
                    }
                    onTranslationChanged: {
                        if (!active)
                            return;
                        // Kept between its neighbours.
                        const low = line.n > 1 ? editor.crossover(line.n - 1) * editor.crossoverGap : editor.minCrossover;
                        const high = line.n < 3 ? editor.crossover(line.n + 1) / editor.crossoverGap : editor.maxCrossover;
                        const f = Math.round(Math.min(Math.max(spans.freqAt(startX + translation.x), low), high));
                        const next = Object.assign({}, editor.live);
                        next["x" + line.n] = f;
                        editor.live = next;
                        editor.set("x" + line.n, f);
                    }
                }
                HoverHandler {
                    id: hover
                    cursorShape: Qt.SizeHorCursor
                }
                QQC2.ToolTip.visible: hover.hovered || drag.active
                QQC2.ToolTip.text: qsTr("Crossover %1: %2 Hz").arg(n).arg(Math.round(freq))
            }
        }
    }

    // --- crossovers as typed values
    RowLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label { text: qsTr("Crossovers") }
        Repeater {
            model: 3
            delegate: ParamField {
                required property int index
                unit: "Hz"
                value: editor.crossover(index + 1)
                accessibleName: qsTr("Crossover %1").arg(index + 1)
                onRequested: v => editor.set("x" + (index + 1), v)
            }
        }
    }

    // --- the bands: a column each
    GridLayout {
        Layout.fillWidth: true
        columns: editor.bandCount + 1
        columnSpacing: Kirigami.Units.smallSpacing
        rowSpacing: Kirigami.Units.smallSpacing

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
                    QQC2.ToolTip.text: qsTr("Off: the band passes without compression")
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
                Item { Layout.fillWidth: true }
                StripToggle {
                    letter: qsTr("S", "solo")
                    text: qsTr("Solo")
                    tip: qsTr("Solo: hear only the soloed bands")
                    onColor: Kirigami.Theme.neutralTextColor
                    on: editor.bandParam(parent.band, "Solo") >= 0.5
                    onRequested: on => editor.set("b" + parent.band + "Solo", on ? 1 : 0)
                }
            }
        }

        component BandRow: Repeater {
            id: row
            required property string field
            required property string unit
            model: editor.bandCount
            delegate: ParamField {
                required property int index
                Layout.fillWidth: true
                unit: row.unit
                value: editor.bandParam(index + 1, row.field)
                accessibleName: qsTr("Band %1 %2").arg(index + 1).arg(row.field.toLowerCase())
                onRequested: v => editor.set("b" + (index + 1) + row.field, v)
            }
        }

        QQC2.Label { text: qsTr("Threshold") }
        BandRow { field: "Threshold"; unit: "dB" }
        QQC2.Label { text: qsTr("Ratio") }
        BandRow { field: "Ratio"; unit: ":1" }
        QQC2.Label { text: qsTr("Attack") }
        BandRow { field: "Attack"; unit: "ms" }
        QQC2.Label { text: qsTr("Release") }
        BandRow { field: "Release"; unit: "ms" }
        QQC2.Label { text: qsTr("Makeup") }
        BandRow { field: "Makeup"; unit: "dB" }
    }
}

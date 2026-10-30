// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// An effect parameter as text you can type into: shows the value with its
// unit ("2.50 kHz", "+3.0 dB", "0.71"); click, type, Enter. Frequencies take
// "2k", "2.5 kHz" or "800". Out-of-range values are clamped by the daemon.
QQC2.TextField {
    id: field

    property real value: 0
    property string unit // "Hz", "dB" or ""
    property string accessibleName

    signal requested(real value)

    function format(v) {
        if (unit === "Hz")
            return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + " kHz" : Math.round(v) + " Hz";
        if (unit === "dB")
            return (v > 0.05 ? "+" : v < -0.05 ? "−" : "") + Math.abs(v).toFixed(1) + " dB";
        return v.toFixed(2);
    }

    // A number from typed text, or NaN.
    function parse(text) {
        let t = text.trim().toLowerCase().replace(/−/g, "-").replace(/\s+/g, "");
        let scale = 1;
        if (unit === "Hz") {
            t = t.replace(/hz$/, "");
            if (t.endsWith("k")) {
                scale = 1000;
                t = t.slice(0, -1);
            }
        } else if (unit === "dB") {
            t = t.replace(/db$/, "");
        }
        if (!/^[+-]?(\d+(\.\d*)?|\.\d+)$/.test(t))
            return NaN;
        return parseFloat(t) * scale;
    }

    implicitWidth: Kirigami.Units.gridUnit * 5
    horizontalAlignment: Text.AlignHCenter
    font.family: "monospace"
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    Accessible.name: accessibleName

    // Shows the daemon's value except while being typed in.
    onValueChanged: {
        if (!activeFocus)
            text = format(value);
    }
    Component.onCompleted: text = format(value)
    onActiveFocusChanged: {
        if (activeFocus)
            selectAll();
        else
            text = format(value);
    }
    onAccepted: {
        const v = parse(text);
        if (isNaN(v)) {
            selectAll();
            return;
        }
        requested(v);
        focus = false;
    }
    Keys.onEscapePressed: focus = false
}

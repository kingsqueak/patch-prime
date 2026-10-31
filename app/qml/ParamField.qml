// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// An effect parameter as text you can type into: shows the value with its
// unit ("2.50 kHz", "+3.0 dB", "12 ms", "4.0:1", "0.71"); click, type,
// Enter. Frequencies take "2k", "2.5 kHz" or "800". Out-of-range values are
// clamped by the daemon.
QQC2.TextField {
    id: field

    property real value: 0
    property string unit // "Hz", "dB", "ms", "s", ":1", "%", "LUFS" or ""
    property string accessibleName

    signal requested(real value)

    function format(v) {
        if (unit === "Hz")
            return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + " kHz" : Math.round(v) + " Hz";
        if (unit === "dB")
            return (v > 0.05 ? "+" : v < -0.05 ? "−" : "") + Math.abs(v).toFixed(1) + " dB";
        if (unit === "ms")
            return (v < 10 ? v.toFixed(1) : Math.round(v)) + " ms";
        if (unit === ":1")
            return v.toFixed(1) + ":1";
        if (unit === "%")
            return Math.round(v) + " %";
        if (unit === "s")
            return v.toFixed(1) + " s";
        if (unit === "LUFS")
            return v.toFixed(1) + " LUFS";
        // Any other unit (a plugin's): as many decimals as the size allows.
        const n = Math.abs(v) >= 100 ? Math.round(v).toString() : v.toFixed(2);
        return unit.length > 0 ? n + " " + unit : n;
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
        } else if (unit === "ms") {
            t = t.replace(/ms$/, "");
        } else if (unit === ":1") {
            t = t.replace(/:1$/, "");
        } else if (unit === "%") {
            t = t.replace(/%$/, "");
        } else if (unit === "s") {
            t = t.replace(/s$/, "");
        } else if (unit === "LUFS") {
            t = t.replace(/lufs$/, "");
        } else if (unit.length > 0) {
            const u = unit.toLowerCase().replace(/\s+/g, "");
            if (t.endsWith(u))
                t = t.slice(0, -u.length);
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

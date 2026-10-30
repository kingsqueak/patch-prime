// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A level in dB that can be typed: shows the value, and a click turns it into
// a text field. Enter applies (`requested(db)`), Escape or clicking away
// cancels. Accepts "-6", "-6 dB", "+3", "−∞" or "-inf" (silent); the value is
// clamped to the fader's range.
Item {
    id: field

    property real value: 0
    property font font: Kirigami.Theme.defaultFont
    property color color: Kirigami.Theme.textColor
    property string accessibleName

    readonly property bool editing: input.visible

    signal requested(real db)

    function format(db) {
        if (db <= Devices.minimumVolumeDb)
            return "−∞";
        const text = Math.abs(db).toFixed(1) + " dB";
        return db > 0.05 ? "+" + text : db < -0.05 ? "−" + text : "0.0 dB";
    }

    // A dB value from typed text, or NaN when it isn't one.
    function parse(text) {
        const t = text.trim().toLowerCase().replace(/−/g, "-").replace(/db$/, "").trim();
        if (t === "-∞" || t === "∞" || t === "-inf" || t === "inf" || t === "off")
            return Devices.minimumVolumeDb;
        if (!/^[+-]?(\d+(\.\d*)?|\.\d+)$/.test(t))
            return NaN;
        return Math.min(Math.max(parseFloat(t), Devices.minimumVolumeDb), Devices.maximumVolumeDb);
    }

    function startEditing() {
        input.text = field.value <= Devices.minimumVolumeDb ? "-inf" : field.value.toFixed(1);
        input.visible = true;
        input.forceActiveFocus();
        input.selectAll();
    }

    implicitWidth: Math.max(display.implicitWidth, Kirigami.Units.gridUnit * 4)
    implicitHeight: display.implicitHeight

    QQC2.Label {
        id: display

        anchors.fill: parent
        visible: !input.visible
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font: field.font
        color: field.color
        text: field.format(field.value)

        HoverHandler {
            id: hover
            cursorShape: Devices.daemonRunning ? Qt.IBeamCursor : Qt.ArrowCursor
        }
        TapHandler {
            enabled: Devices.daemonRunning
            onTapped: field.startEditing()
        }
        QQC2.ToolTip.text: qsTr("Click to type a level")
        QQC2.ToolTip.visible: hover.hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    QQC2.TextField {
        id: input

        anchors.centerIn: parent
        width: parent.width
        height: Math.max(parent.height, implicitHeight)
        visible: false
        horizontalAlignment: Text.AlignHCenter
        font: field.font
        topPadding: 0
        bottomPadding: 0
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        Accessible.name: field.accessibleName

        onAccepted: {
            const db = field.parse(text);
            if (isNaN(db)) {
                selectAll(); // not a number: let it be fixed
                return;
            }
            visible = false;
            field.requested(db);
        }
        Keys.onEscapePressed: visible = false
        onActiveFocusChanged: {
            if (!activeFocus)
                visible = false;
        }
    }
}

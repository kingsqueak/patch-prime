// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// One effect parameter, by its kind: a switch (toggle), a menu (choice), or
// a slider with a field to type into (on a log scale for frequencies and
// times). Double-click the slider for the initial value. Used for built-in
// effects without their own editor, and for plugins.
RowLayout {
    id: row

    required property string stripKey
    required property int effectIndex
    required property var info   // {name, label, unit, minimum, maximum, initial, kind, options}
    required property var values // the effect's parameters, by name

    readonly property real current: {
        const v = values[info.name];
        return v === undefined ? info.initial : v;
    }
    readonly property bool log: info.kind === "log" && info.minimum > 0

    function set(v) {
        Devices.setEffectParam(row.stripKey, row.effectIndex, row.info.name, v);
    }
    // Slider position (0-1) <-> value.
    function toPosition(v) {
        const lo = info.minimum;
        const hi = info.maximum;
        if (hi <= lo)
            return 0;
        return log ? Math.log(v / lo) / Math.log(hi / lo) : (v - lo) / (hi - lo);
    }
    function toValue(position) {
        const lo = info.minimum;
        const hi = info.maximum;
        const v = log ? lo * Math.pow(hi / lo, position) : lo + position * (hi - lo);
        if (Math.abs(v) >= 100)
            return Math.round(v);
        return Number(v.toPrecision(3));
    }

    Layout.fillWidth: true

    QQC2.Label {
        Layout.preferredWidth: Kirigami.Units.gridUnit * 7
        text: row.info.label
        elide: Text.ElideRight
        QQC2.ToolTip.text: row.info.label
        QQC2.ToolTip.visible: truncated && hoverHandler.hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

        HoverHandler {
            id: hoverHandler
        }
    }

    QQC2.Switch {
        visible: row.info.kind === "toggle"
        checked: row.current >= 0.5
        onToggled: row.set(checked ? 1 : 0)
        Accessible.name: row.info.label
    }

    QQC2.ComboBox {
        Layout.fillWidth: true
        visible: row.info.kind === "choice"
        model: row.info.options || []
        currentIndex: Math.round(row.current - row.info.minimum)
        onActivated: index => row.set(row.info.minimum + index)
        Accessible.name: row.info.label
    }

    Item {
        Layout.fillWidth: true
        visible: row.info.kind === "toggle"
    }

    QQC2.Slider {
        id: slider

        Layout.fillWidth: true
        visible: row.info.kind !== "toggle" && row.info.kind !== "choice"
        from: 0
        to: 1
        onMoved: row.set(row.toValue(value))
        Accessible.name: row.info.label
        // Follow the daemon as VolumeFader does.
        Connections {
            target: row
            function onCurrentChanged() {
                if (!slider.pressed)
                    slider.value = row.toPosition(row.current);
            }
        }
        onPressedChanged: {
            if (!pressed)
                value = row.toPosition(row.current);
        }
        Component.onCompleted: value = row.toPosition(row.current)

        TapHandler {
            onDoubleTapped: row.set(row.info.initial)
        }
    }

    ParamField {
        visible: slider.visible
        unit: row.info.unit
        value: slider.pressed ? row.toValue(slider.value) : row.current
        accessibleName: row.info.label
        onRequested: v => row.set(v)
    }
}

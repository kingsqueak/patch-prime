// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's effects, in a window of its own beside the mixer (so neither
// has to be resized for the other): the chain in order, each effect with
// on/bypass, move, remove and its parameters; and a menu to add one. On a
// hardware input the effects run before the fader (and the system mic gets
// them); elsewhere, after it. Each effect has a meter bridge (in, gain
// reduction, out), measured only while the window is open.
QQC2.ApplicationWindow {
    id: panel

    property string key
    property string label
    property var effects: []

    // The parameter list for a type, from the daemon's effectTypes.
    function typeInfo(type) {
        const types = Devices.effectTypes;
        for (let i = 0; i < types.length; ++i) {
            if (types[i].type === type)
                return types[i];
        }
        return { type: type, label: type, params: [] };
    }

    // Shows the window (or brings it forward), sized for its editors.
    function open() {
        if (!visible) {
            const wide = panel.effects.some(e => e.type === "eq" || e.type === "multiband");
            width = Kirigami.Units.gridUnit * (wide ? 60 : 36);
            height = Math.min(Screen.desktopAvailableHeight * 0.85, Kirigami.Units.gridUnit * 44);
        }
        show();
        raise();
        requestActivate();
    }

    title: qsTr("Effects: %1").arg(panel.label)
    // The effects' input and output levels cost CPU: only while they're seen.
    onVisibleChanged: Devices.watchEffectLevels(panel.key, visible)
    Component.onDestruction: {
        if (visible)
            Devices.watchEffectLevels(panel.key, false);
    }

    minimumWidth: Kirigami.Units.gridUnit * 28
    minimumHeight: Kirigami.Units.gridUnit * 16
    color: Kirigami.Theme.backgroundColor

    QQC2.ScrollView {
        id: scroll

        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        contentWidth: availableWidth

    ColumnLayout {
        width: scroll.availableWidth
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            visible: panel.effects.length === 0
            wrapMode: Text.Wrap
            color: Style.dimText
            text: qsTr("No effects. They run in order, top to bottom: before the fader on an input, after it elsewhere. A strip without effects has no processing at all.")
        }

        Repeater {
            model: panel.effects

            delegate: QQC2.Frame {
                id: slot

                required property var modelData
                required property int index
                readonly property var info: panel.typeInfo(modelData.type)

                Layout.fillWidth: true
                leftPadding: Kirigami.Units.largeSpacing + Style.stageBarHeight

                // A card, with a bar down the left: lit while the effect is on.
                Kirigami.Theme.colorSet: Kirigami.Theme.View
                Kirigami.Theme.inherit: false
                background: Rectangle {
                    color: Kirigami.Theme.backgroundColor
                    radius: Style.stripRadius
                    border.color: Style.stripBorder

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.margins: 1
                        width: Style.stageBarHeight
                        radius: Style.stripRadius
                        color: slot.modelData.enabled ? Style.slotOn : Style.slotOff
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        Layout.fillWidth: true

                        // A lit power button: on, or bypassed.
                        StripToggle {
                            icon.name: "system-shutdown-symbolic"
                            text: qsTr("%1 on").arg(slot.info.label)
                            tip: slot.modelData.enabled ? qsTr("On (click to bypass)") : qsTr("Bypassed (click to turn on)")
                            on: slot.modelData.enabled
                            enabled: Devices.daemonRunning
                            onRequested: on => Devices.setEffectEnabled(panel.key, slot.index, on)
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: (slot.index + 1) + ". " + slot.info.label
                            font.bold: true
                            opacity: slot.modelData.enabled ? 1 : 0.6
                        }

                        QQC2.ToolButton {
                            icon.name: "go-up"
                            enabled: slot.index > 0
                            onClicked: Devices.moveEffect(panel.key, slot.index, slot.index - 1)
                            Accessible.name: qsTr("Move up")
                        }
                        QQC2.ToolButton {
                            icon.name: "go-down"
                            enabled: slot.index < panel.effects.length - 1
                            onClicked: Devices.moveEffect(panel.key, slot.index, slot.index + 1)
                            Accessible.name: qsTr("Move down")
                        }
                        QQC2.ToolButton {
                            icon.name: "edit-delete-remove"
                            onClicked: Devices.removeEffect(panel.key, slot.index)
                            Accessible.name: qsTr("Remove %1").arg(slot.info.label)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: Kirigami.Units.smallSpacing

                            // The EQ and the multiband compressor have their own
                            // editors; other effects get a row per parameter.
                            Loader {
                                Layout.fillWidth: true
                                active: slot.modelData.type === "eq"
                                visible: active
                                sourceComponent: EqEditor {
                                    key: panel.key
                                    index: slot.index
                                    params: slot.modelData.params
                                }
                            }

                            Loader {
                                Layout.fillWidth: true
                                active: slot.modelData.type === "multiband"
                                visible: active
                                sourceComponent: MultibandEditor {
                                    key: panel.key
                                    index: slot.index
                                    params: slot.modelData.params
                                }
                            }

                            Repeater {
                                model: slot.modelData.type === "eq" || slot.modelData.type === "multiband"
                                       ? [] : slot.info.params

                                delegate: RowLayout {
                                    id: param

                                    required property var modelData
                                    readonly property real current: {
                                        const v = slot.modelData.params[modelData.name];
                                        return v === undefined ? modelData.initial : v;
                                    }
                                    readonly property bool log: modelData.kind === "log" && modelData.minimum > 0

                                    // Slider position (0-1) <-> value, on a log scale for times and frequencies.
                                    function toPosition(v) {
                                        const lo = modelData.minimum;
                                        const hi = modelData.maximum;
                                        return log ? Math.log(v / lo) / Math.log(hi / lo) : (v - lo) / (hi - lo);
                                    }
                                    function toValue(position) {
                                        const lo = modelData.minimum;
                                        const hi = modelData.maximum;
                                        const v = log ? lo * Math.pow(hi / lo, position) : lo + position * (hi - lo);
                                        return Math.round(v * (Math.abs(v) < 10 ? 10 : 1)) / (Math.abs(v) < 10 ? 10 : 1);
                                    }

                                    Layout.fillWidth: true

                                    QQC2.Label {
                                        Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                                        text: param.modelData.label
                                        elide: Text.ElideRight
                                    }

                                    QQC2.Slider {
                                        id: slider

                                        Layout.fillWidth: true
                                        from: 0
                                        to: 1
                                        onMoved: Devices.setEffectParam(panel.key, slot.index,
                                                                        param.modelData.name,
                                                                        param.toValue(value))
                                        Accessible.name: param.modelData.label
                                        // Follow the daemon as VolumeFader does.
                                        Connections {
                                            target: param
                                            function onCurrentChanged() {
                                                if (!slider.pressed)
                                                    slider.value = param.toPosition(param.current);
                                            }
                                        }
                                        onPressedChanged: {
                                            if (!pressed)
                                                value = param.toPosition(param.current);
                                        }
                                        Component.onCompleted: value = param.toPosition(param.current)

                                        TapHandler {
                                            onDoubleTapped: Devices.setEffectParam(panel.key, slot.index,
                                                                                   param.modelData.name,
                                                                                   param.modelData.initial)
                                        }
                                    }

                                    ParamField {
                                        unit: param.modelData.unit
                                        value: slider.pressed ? param.toValue(slider.value) : param.current
                                        accessibleName: param.modelData.label
                                        onRequested: v => Devices.setEffectParam(panel.key, slot.index,
                                                                                 param.modelData.name, v)
                                    }
                                }
                            }

                            // A ducker's key: the strip whose sound turns this one down.
                            RowLayout {
                                Layout.fillWidth: true
                                visible: slot.info.takesKey === true

                                QQC2.Label {
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                                    text: qsTr("Key")
                                }
                                QQC2.ComboBox {
                                    id: keyChoice

                                    Layout.fillWidth: true
                                    model: StripFilter {
                                        sourceModel: Devices
                                        stages: ["input", "bus", "output"]
                                    }
                                    textRole: "label"
                                    valueRole: "key"
                                    displayText: (slot.modelData.key || "").length > 0
                                                 ? Devices.labelFor(slot.modelData.key)
                                                 : qsTr("None: choose the strip to listen to")
                                    currentIndex: indexOfValue(slot.modelData.key || "")
                                    onActivated: Devices.setEffectKey(panel.key, slot.index, currentValue)
                                    Accessible.name: qsTr("Key strip")
                                    QQC2.ToolTip.text: qsTr("Turn this strip down while the chosen strip is above the threshold (e.g. music under the mic). A strip that this one feeds can't be chosen: that would be a loop.")
                                    QQC2.ToolTip.visible: hovered
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                }
                                QQC2.ToolButton {
                                    icon.name: "edit-clear"
                                    enabled: (slot.modelData.key || "").length > 0
                                    onClicked: Devices.setEffectKey(panel.key, slot.index, "")
                                    Accessible.name: qsTr("No key")
                                }
                            }

                            // Meters beyond the bridge: the voice level, the leveler's loudness.
                            Repeater {
                                id: meters

                                readonly property var values: Devices.effectMeters[panel.key + "|" + slot.index] || []

                                model: ({
                                    denoise: [{ label: qsTr("Voice"), unit: "%", from: 0, to: 100, at: 0 }],
                                    leveler: [{ label: qsTr("Loudness"), unit: "LUFS", from: -60, to: 0, at: 1 }]
                                })[slot.modelData.type] || []

                                delegate: RowLayout {
                                    required property var modelData
                                    required property int index
                                    readonly property real value: modelData.at < meters.values.length
                                                                  ? meters.values[modelData.at] : modelData.from

                                    Layout.fillWidth: true

                                    QQC2.Label {
                                        Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                                        text: parent.modelData.label
                                    }
                                    QQC2.ProgressBar {
                                        Layout.fillWidth: true
                                        from: parent.modelData.from
                                        to: parent.modelData.to
                                        value: parent.value
                                    }
                                    QQC2.Label {
                                        Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                                        horizontalAlignment: Text.AlignHCenter
                                        font.family: "monospace"
                                        text: parent.value.toFixed(parent.modelData.unit === "%" ? 0 : 1)
                                              + " " + parent.modelData.unit
                                    }
                                }
                            }
                        }

                        EffectMeterBridge {
                            Layout.fillHeight: true
                            Layout.minimumHeight: Style.fxBridgeHeight
                            Layout.alignment: Qt.AlignTop
                            key: panel.key
                            index: slot.index
                            type: slot.modelData.type
                            on: slot.modelData.enabled
                            params: slot.modelData.params
                            paramInfo: slot.info.params
                        }
                    }
                }
            }
        }

        QQC2.Button {
            text: qsTr("Add Effect")
            icon.name: "list-add"
            enabled: Devices.daemonRunning && panel.effects.length < 8
            onClicked: addMenu.popup(this, 0, height)

            QQC2.Menu {
                id: addMenu

                Instantiator {
                    model: Devices.effectTypes

                    delegate: QQC2.MenuItem {
                        required property var modelData
                        text: modelData.label
                        onTriggered: Devices.addEffect(panel.key, modelData.type)
                    }

                    onObjectAdded: (index, object) => addMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => addMenu.removeItem(object)
                }
            }
        }
    }
    }
}

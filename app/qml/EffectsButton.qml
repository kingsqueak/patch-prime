// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's "FX" button, lit while any effect is on, and the window it opens
// (a window of its own, beside the mixer, so neither has to be resized for
// the other): the strip's effects chain in order, each with on/bypass, move,
// remove and its parameters; and a menu to add one. On a hardware input the
// effects run before the fader (and the system mic gets them); elsewhere,
// after it.
StripToggle {
    id: button

    property string key
    property string label
    property var effects: []

    readonly property int activeCount: effects.filter(e => e.enabled).length

    letter: qsTr("FX", "effects button")
    text: qsTr("Effects")
    checkable: false
    on: activeCount > 0
    tip: effects.length === 0 ? qsTr("Add effects to %1").arg(label)
        : qsTr("%1 effects, %2 on").arg(effects.length).arg(activeCount)
    onClicked: panel.open()

    // Developer aid (PATCHPRIME_EFFECTS): open this strip's panel at start.
    onLabelChanged: {
        if (label.length > 0 && QQC2.ApplicationWindow.window
                && QQC2.ApplicationWindow.window.openEffects === label)
            panel.open();
    }

    // The parameter list for a type, from the daemon's effectTypes.
    function typeInfo(type) {
        const types = Devices.effectTypes;
        for (let i = 0; i < types.length; ++i) {
            if (types[i].type === type)
                return types[i];
        }
        return { type: type, label: type, params: [] };
    }

    QQC2.ApplicationWindow {
        id: panel

        // Shows the window (or brings it forward), sized for its editors.
        function open() {
            if (!visible) {
                const wide = button.effects.some(e => e.type === "eq" || e.type === "multiband");
                width = Kirigami.Units.gridUnit * (wide ? 56 : 30);
                height = Math.min(Screen.desktopAvailableHeight * 0.85, Kirigami.Units.gridUnit * 44);
            }
            show();
            raise();
            requestActivate();
        }

        title: qsTr("Effects: %1 — Patch Prime").arg(button.label)
        minimumWidth: Kirigami.Units.gridUnit * 24
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
                visible: button.effects.length === 0
                wrapMode: Text.Wrap
                color: Style.dimText
                text: qsTr("No effects. They run in order, top to bottom: before the fader on an input, after it elsewhere. A strip without effects has no processing at all.")
            }

            Repeater {
                model: button.effects

                delegate: QQC2.Frame {
                    id: slot

                    required property var modelData
                    required property int index
                    readonly property var info: button.typeInfo(modelData.type)

                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            Layout.fillWidth: true

                            QQC2.Switch {
                                checked: slot.modelData.enabled
                                onToggled: Devices.setEffectEnabled(button.key, slot.index, checked)
                                Accessible.name: qsTr("%1 on").arg(slot.info.label)
                                QQC2.ToolTip.text: checked ? qsTr("On (click to bypass)") : qsTr("Bypassed")
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
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
                                onClicked: Devices.moveEffect(button.key, slot.index, slot.index - 1)
                                Accessible.name: qsTr("Move up")
                            }
                            QQC2.ToolButton {
                                icon.name: "go-down"
                                enabled: slot.index < button.effects.length - 1
                                onClicked: Devices.moveEffect(button.key, slot.index, slot.index + 1)
                                Accessible.name: qsTr("Move down")
                            }
                            QQC2.ToolButton {
                                icon.name: "edit-delete-remove"
                                onClicked: Devices.removeEffect(button.key, slot.index)
                                Accessible.name: qsTr("Remove %1").arg(slot.info.label)
                            }
                        }

                        // The EQ and the multiband compressor have their own
                        // editors; other effects get a row per parameter.
                        Loader {
                            Layout.fillWidth: true
                            active: slot.modelData.type === "eq"
                            visible: active
                            sourceComponent: EqEditor {
                                key: button.key
                                index: slot.index
                                params: slot.modelData.params
                            }
                        }

                        Loader {
                            Layout.fillWidth: true
                            active: slot.modelData.type === "multiband"
                            visible: active
                            sourceComponent: MultibandEditor {
                                key: button.key
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
                                    onMoved: Devices.setEffectParam(button.key, slot.index,
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
                                        onDoubleTapped: Devices.setEffectParam(button.key, slot.index,
                                                                               param.modelData.name,
                                                                               param.modelData.initial)
                                    }
                                }

                                ParamField {
                                    unit: param.modelData.unit
                                    value: slider.pressed ? param.toValue(slider.value) : param.current
                                    accessibleName: param.modelData.label
                                    onRequested: v => Devices.setEffectParam(button.key, slot.index,
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
                                onActivated: Devices.setEffectKey(button.key, slot.index, currentValue)
                                Accessible.name: qsTr("Key strip")
                                QQC2.ToolTip.text: qsTr("Turn this strip down while the chosen strip is above the threshold (e.g. music under the mic). A strip that this one feeds can't be chosen: that would be a loop.")
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            }
                            QQC2.ToolButton {
                                icon.name: "edit-clear"
                                enabled: (slot.modelData.key || "").length > 0
                                onClicked: Devices.setEffectKey(button.key, slot.index, "")
                                Accessible.name: qsTr("No key")
                            }
                        }

                        // Effects with meters: the gate's cut, the ducker's dip, the voice
                        // level, the leveler's gain and loudness, the limiting.
                        Repeater {
                            id: meters

                            readonly property var values: Devices.effectMeters[button.key + "|" + slot.index] || []

                            model: ({
                                gate: [{ label: qsTr("Gate cut"), unit: "dB", from: 0, to: 40 }],
                                ducker: [{ label: qsTr("Ducking"), unit: "dB", from: 0, to: 24 }],
                                denoise: [{ label: qsTr("Voice"), unit: "%", from: 0, to: 100 }],
                                leveler: [{ label: qsTr("Gain now"), unit: "dB", from: -24, to: 24 },
                                          { label: qsTr("Loudness"), unit: "LUFS", from: -60, to: 0 }],
                                limiter: [{ label: qsTr("Limiting"), unit: "dB", from: 0, to: 12 }]
                            })[slot.modelData.type] || []

                            delegate: RowLayout {
                                required property var modelData
                                required property int index
                                readonly property real value: index < meters.values.length ? meters.values[index]
                                                                                           : modelData.from

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
                }
            }

            QQC2.Button {
                text: qsTr("Add Effect")
                icon.name: "list-add"
                enabled: Devices.daemonRunning && button.effects.length < 8
                onClicked: addMenu.popup(this, 0, height)

                QQC2.Menu {
                    id: addMenu

                    Instantiator {
                        model: Devices.effectTypes

                        delegate: QQC2.MenuItem {
                            required property var modelData
                            text: modelData.label
                            onTriggered: Devices.addEffect(button.key, modelData.type)
                        }

                        onObjectAdded: (index, object) => addMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => addMenu.removeItem(object)
                    }
                }
            }
        }
        }
    }
}

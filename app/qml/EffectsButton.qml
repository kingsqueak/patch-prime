// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's "FX" button, lit while any effect is on, and the panel it opens:
// the strip's effects chain in order, each with on/bypass, move, remove and
// its parameters; and a menu to add one. On a hardware input the effects run
// before the fader (and the system mic gets them); elsewhere, after it.
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

    // The parameter list for a type, from the daemon's effectTypes.
    function typeInfo(type) {
        const types = Devices.effectTypes;
        for (let i = 0; i < types.length; ++i) {
            if (types[i].type === type)
                return types[i];
        }
        return { type: type, label: type, params: [] };
    }

    function formatValue(value, unit) {
        const text = (Math.abs(value) < 10 ? value.toFixed(1) : value.toFixed(0));
        return unit.length > 0 ? text + " " + unit : text;
    }

    Kirigami.Dialog {
        id: panel

        title: qsTr("Effects: %1").arg(button.label)
        standardButtons: Kirigami.Dialog.Close
        preferredWidth: Kirigami.Units.gridUnit * 24
        padding: Kirigami.Units.largeSpacing

        ColumnLayout {
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

                        Repeater {
                            model: slot.info.params

                            delegate: RowLayout {
                                id: param

                                required property var modelData
                                readonly property real current: {
                                    const v = slot.modelData.params[modelData.name];
                                    return v === undefined ? modelData.initial : v;
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
                                    from: param.modelData.minimum
                                    to: param.modelData.maximum
                                    onMoved: Devices.setEffectParam(button.key, slot.index,
                                                                    param.modelData.name,
                                                                    Math.round(value * 10) / 10)
                                    Accessible.name: param.modelData.label
                                    // Follow the daemon as VolumeFader does.
                                    Connections {
                                        target: param
                                        function onCurrentChanged() {
                                            if (!slider.pressed)
                                                slider.value = param.current;
                                        }
                                    }
                                    onPressedChanged: {
                                        if (!pressed)
                                            value = param.current;
                                    }
                                    Component.onCompleted: value = param.current

                                    TapHandler {
                                        onDoubleTapped: Devices.setEffectParam(button.key, slot.index,
                                                                               param.modelData.name,
                                                                               param.modelData.initial)
                                    }
                                }

                                QQC2.Label {
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 4
                                    horizontalAlignment: Text.AlignRight
                                    font.family: "monospace"
                                    text: button.formatValue(slider.pressed ? slider.value : param.current,
                                                             param.modelData.unit)
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

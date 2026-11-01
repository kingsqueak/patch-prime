// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls
import org.kingsqueak.patchprime.engine

// Makes a global hotkey: what it does, to which strip (or scene), and its
// key. With `fixedTarget` set (from a control's right-click menu), only
// what suits that target is offered.
Kirigami.Dialog {
    id: dialog

    property var fixedTarget: null
    property bool destroyOnClose: false

    // Everything a hotkey can do from the Devices page: {text, mode, action}.
    readonly property var allChoices: [
        { text: qsTr("Mute on/off"), mode: "toggle", action: "mute" },
        { text: qsTr("Push to talk (unmuted while held)"), mode: "pushToTalk", action: "mute" },
        { text: qsTr("Push to mute (muted while held)"), mode: "pushToMute", action: "mute" },
        { text: qsTr("Cue on/off"), mode: "toggle", action: "cue" },
        { text: qsTr("Record on/off"), mode: "toggle", action: "record" },
        { text: qsTr("Duck while held (%1 dB down)").arg(15), mode: "duck", action: "volume" },
        { text: qsTr("Level up 1 dB"), mode: "levelUp", action: "volume" },
        { text: qsTr("Level down 1 dB"), mode: "levelDown", action: "volume" },
        { text: qsTr("Load a scene"), mode: "toggle", action: "scene" },
    ]
    readonly property var choices: {
        if (!fixedTarget)
            return allChoices;
        const a = fixedTarget.action;
        if (a === "mute" || a === "volume")
            return allChoices.filter(c => c.action === a);
        if (a === "sendLevel")
            return [{ text: qsTr("Level up 1 dB"), mode: "levelUp", action: a },
                    { text: qsTr("Level down 1 dB"), mode: "levelDown", action: a }];
        if (a === "pad")
            return [{ text: qsTr("Play / stop"), mode: "toggle", action: a }];
        return [{ text: qsTr("On/off"), mode: "toggle", action: a }];
    }
    readonly property var choice: choices[Math.max(0, modeChoice.currentIndex)]
    readonly property var target: {
        if (fixedTarget)
            return fixedTarget;
        if (choice.action === "scene")
            return { action: "scene", scene: sceneChoice.currentText };
        return { action: choice.action, key: stripChoice.currentValue || "" };
    }
    readonly property bool complete: target.action === "scene" ? (target.scene || "") !== "" : (target.key || "") !== ""

    title: qsTr("Add Hotkey")
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.NoButton
    customFooterActions: [
        Kirigami.Action {
            text: qsTr("Add")
            icon.name: "list-add"
            enabled: dialog.complete
            onTriggered: {
                Devices.addHotkey({ mode: dialog.choice.mode, target: dialog.target }, keyItem.keySequence);
                dialog.close();
            }
        },
        Kirigami.Action {
            text: qsTr("Cancel")
            icon.name: "dialog-cancel"
            onTriggered: dialog.close()
        }
    ]
    onClosed: {
        if (destroyOnClose)
            destroy();
    }

    StripFilter {
        id: strips
        sourceModel: Devices
        stages: ["input", "bus", "output"]
    }

    Kirigami.FormLayout {
        QQC2.Label {
            visible: dialog.fixedTarget !== null
            Kirigami.FormData.label: qsTr("For:")
            text: dialog.fixedTarget ? Devices.describeTarget(dialog.fixedTarget) : ""
        }

        QQC2.ComboBox {
            id: modeChoice
            Kirigami.FormData.label: qsTr("Does:")
            visible: dialog.choices.length > 1
            model: dialog.choices
            textRole: "text"
            Layout.preferredWidth: Kirigami.Units.gridUnit * 16
        }

        QQC2.ComboBox {
            id: stripChoice
            Kirigami.FormData.label: qsTr("Strip:")
            visible: !dialog.fixedTarget && dialog.choice.action !== "scene"
            model: strips
            textRole: "label"
            valueRole: "key"
            Layout.preferredWidth: Kirigami.Units.gridUnit * 16
        }

        QQC2.ComboBox {
            id: sceneChoice
            Kirigami.FormData.label: qsTr("Scene:")
            visible: !dialog.fixedTarget && dialog.choice.action === "scene"
            model: Devices.scenes
            Layout.preferredWidth: Kirigami.Units.gridUnit * 16
        }

        KQuickControls.KeySequenceItem {
            id: keyItem
            Kirigami.FormData.label: qsTr("Key:")
            modifierlessAllowed: true // F13, Pause, a spare key for push to talk
            checkForConflictsAgainst: KQuickControls.ShortcutType.GlobalShortcuts | KQuickControls.ShortcutType.StandardShortcuts
        }

        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: qsTr("Works in every app. Keys can also be changed later here or in System Settings → Shortcuts → Patch Prime.")
        }
    }
}

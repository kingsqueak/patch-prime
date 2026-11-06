// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// The first-run wizard: which mic, which headphones, what goes through the
// mixer, do you stream. Finish hands the answers to the daemon in one call
// (ApplyQuickSetup), which only adds; nothing happens before that.
Kirigami.Dialog {
    id: wizard

    property var mics: []
    property var outputs: []

    readonly property string none: "" // "no microphone": an empty key
    readonly property int lastStep: pages.count - 1
    readonly property string micLabel: micChoice.currentIndex >= 0 ? micChoice.currentText : ""
    readonly property string outputLabel: outputChoice.currentIndex >= 0 ? outputChoice.currentText : ""

    function refresh() {
        mics = Devices.hardwareDevices("input").concat([{ key: none, label: qsTr("No microphone") }]);
        outputs = Devices.hardwareDevices("output");
        micChoice.currentIndex = Math.max(0, mics.findIndex(d => d.isDefault));
        outputChoice.currentIndex = Math.max(0, outputs.findIndex(d => d.isDefault));
    }

    function start(step) {
        refresh();
        pages.currentIndex = step || 0;
        open();
    }

    function finish() {
        Devices.applyQuickSetup(micChoice.currentValue || "",
                                outputChoice.currentValue || "",
                                desktopChoice.checked ? qsTr("Desktop") : "",
                                streamChoice.checked ? qsTr("Stream Mix") : "");
        close();
    }

    Connections {
        target: Devices
        // Opened before the devices arrived.
        function onRowsInserted() {
            if (wizard.opened && wizard.outputs.length === 0)
                wizard.refresh();
        }
        function onModelReset() {
            if (wizard.opened && wizard.outputs.length === 0)
                wizard.refresh();
        }
    }

    title: qsTr("Set Up Patch Prime")
    preferredWidth: Kirigami.Units.gridUnit * 30
    maximumWidth: Kirigami.Units.gridUnit * 30
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.NoButton
    closePolicy: QQC2.Popup.CloseOnEscape

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("Skip")
            icon.name: "dialog-cancel"
            visible: pages.currentIndex < wizard.lastStep
            onTriggered: wizard.close()
        },
        Kirigami.Action {
            text: qsTr("Back")
            icon.name: "go-previous"
            enabled: pages.currentIndex > 0
            onTriggered: pages.currentIndex -= 1
        },
        Kirigami.Action {
            text: pages.currentIndex < wizard.lastStep ? qsTr("Next") : qsTr("Finish")
            icon.name: pages.currentIndex < wizard.lastStep ? "go-next" : "dialog-ok-apply"
            onTriggered: {
                if (pages.currentIndex < wizard.lastStep)
                    pages.currentIndex += 1;
                else
                    wizard.finish();
            }
        }
    ]

    StackLayout {
        id: pages

        // A page of the wizard: a heading, and its words under it.
        component Step: ColumnLayout {
            property string heading
            property string words
            default property alias content: extra.data

            spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 3
                text: parent.heading
                wrapMode: Text.Wrap
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: parent.words
                wrapMode: Text.Wrap
            }
            ColumnLayout {
                id: extra
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
            }
            Item { Layout.fillHeight: true }
        }

        Step {
            heading: qsTr("Welcome")
            words: qsTr("A few questions will get your mic and headphones into the mixer. Nothing changes until you press Finish, and you can change all of it later.")
        }

        Step {
            heading: qsTr("Which microphone do you use?")
            words: qsTr("It goes in the mixer, and apps record it from there, with any effects you add.")

            QQC2.ComboBox {
                id: micChoice
                Layout.fillWidth: true
                Accessible.name: qsTr("Microphone")
                model: wizard.mics
                textRole: "label"
                valueRole: "key"
            }
        }

        Step {
            heading: qsTr("Where do you listen?")
            words: qsTr("Your headphones or speakers.")

            QQC2.ComboBox {
                id: outputChoice
                Layout.fillWidth: true
                Accessible.name: qsTr("Headphones or speakers")
                model: wizard.outputs
                textRole: "label"
                valueRole: "key"
            }
        }

        Step {
            heading: qsTr("Desktop sound")
            words: qsTr("With this, what your apps play goes through a Desktop strip, so you have one fader for it and can send it to a stream. Without it, apps play to your output directly, through its fader.")

            QQC2.CheckBox {
                id: desktopChoice
                Layout.fillWidth: true
                checked: true
                text: qsTr("Play desktop sound through the mixer")
            }
        }

        Step {
            heading: qsTr("Do you stream or record?")
            words: qsTr("A Stream Mix bus gets your mic and desktop sound, for OBS or another app to capture, with its own levels.")

            QQC2.CheckBox {
                id: streamChoice
                Layout.fillWidth: true
                text: qsTr("Add a mix for streaming and recording")
            }
        }

        Step {
            heading: qsTr("Ready")
            words: qsTr("Press Finish to set this up. You won't hear your own mic: to hear it, send its strip to your output.")

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Microphone: %1").arg(wizard.micLabel)
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Output: %1").arg(wizard.outputLabel)
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: desktopChoice.checked
                text: qsTr("Desktop sound goes through the mixer.")
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: streamChoice.checked
                text: qsTr("A Stream Mix bus for streaming and recording.")
            }
        }
    }
}

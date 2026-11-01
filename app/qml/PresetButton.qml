// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A Presets button: its menu saves what's there under a name, loads one of
// `names`, or deletes one. Used for whole effects chains and for single
// effects; the owner does the saving and loading.
QQC2.ToolButton {
    id: button

    property var names: []
    property string saveTitle: qsTr("Save Preset")
    property string loadHint // the menu's note on what loading does

    signal save(string name)
    signal load(string name)
    signal remove(string name)

    icon.name: "bookmarks"
    display: QQC2.AbstractButton.TextBesideIcon
    enabled: Devices.daemonRunning
    onClicked: menu.popup(button, 0, button.height)

    QQC2.Menu {
        id: menu

        QQC2.MenuItem {
            text: qsTr("Save as Preset…")
            icon.name: "document-save-as"
            onTriggered: {
                const host = button.QQC2.ApplicationWindow.window;
                nameDialog.createObject(host ? host.contentItem : button).open();
            }
        }

        QQC2.MenuSeparator {}

        QQC2.MenuItem {
            enabled: false
            text: button.names.length === 0 ? qsTr("No presets yet") : button.loadHint
            visible: text.length > 0
            height: visible ? implicitHeight : 0
        }

        Instantiator {
            model: button.names

            delegate: QQC2.MenuItem {
                required property string modelData
                text: modelData
                icon.name: "document-open"
                onTriggered: button.load(modelData)
            }

            onObjectAdded: (index, object) => menu.insertItem(3 + index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }

        QQC2.Menu {
            id: deleteMenu
            title: qsTr("Delete")
            icon.name: "edit-delete-remove"
            enabled: button.names.length > 0

            Instantiator {
                model: button.names

                delegate: QQC2.MenuItem {
                    required property string modelData
                    text: modelData
                    onTriggered: button.remove(modelData)
                }

                onObjectAdded: (index, object) => deleteMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => deleteMenu.removeItem(object)
            }
        }
    }

    Component {
        id: nameDialog

        Kirigami.PromptDialog {
            id: dialog
            title: button.saveTitle
            standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel
            onOpened: {
                nameField.forceActiveFocus();
                nameField.selectAll();
            }
            onAccepted: {
                if (nameField.text.trim().length > 0)
                    button.save(nameField.text.trim());
            }
            onClosed: destroy()

            QQC2.TextField {
                id: nameField
                placeholderText: qsTr("Name")
                onAccepted: dialog.accept()
            }

            QQC2.Label {
                visible: button.names.includes(nameField.text.trim())
                text: qsTr("Replaces the preset with that name.")
                color: Kirigami.Theme.neutralTextColor
                font: Kirigami.Theme.smallFont
            }
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Choose a plugin to add to a strip: the installed LV2, CLAP and VST3 plugins,
// searchable by name, maker, category or format, and filterable by category. Plugins that
// can't run here (instruments, ones needing something Patch Prime doesn't
// provide) are listed greyed out, with the reason.
QQC2.Dialog {
    id: picker

    signal chosen(string type)

    readonly property var categories: {
        const set = new Set();
        for (const p of Devices.plugins)
            set.add(p.category);
        return [qsTr("All categories")].concat(Array.from(set).sort());
    }
    readonly property var shown: {
        const words = search.text.toLowerCase().split(/\s+/).filter(w => w.length > 0);
        const category = categoryChoice.currentIndex > 0 ? categories[categoryChoice.currentIndex] : "";
        return Devices.plugins.filter(p => {
            if (category.length > 0 && p.category !== category)
                return false;
            if (!showAll.checked && p.problem.length > 0)
                return false;
            const text = (p.name + " " + p.author + " " + p.category + " " + picker.format(p)).toLowerCase();
            return words.every(w => text.includes(w));
        });
    }

    function format(plugin) {
        return plugin.type.split(":")[0].toUpperCase(); // LV2, CLAP, VST3
    }
    function choose(plugin) {
        if (plugin.problem.length > 0)
            return;
        chosen(plugin.type);
        close();
    }

    title: qsTr("Add Plugin")
    modal: true
    anchors.centerIn: QQC2.Overlay.overlay
    width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 34)
    height: Math.min(parent.height - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 36)
    standardButtons: QQC2.Dialog.Cancel
    onOpened: {
        search.text = "";
        search.forceActiveFocus();
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true

            Kirigami.SearchField {
                id: search

                Layout.fillWidth: true
                placeholderText: qsTr("Search plugins")
                Keys.onReturnPressed: {
                    if (picker.shown.length > 0)
                        picker.choose(picker.shown[Math.max(list.currentIndex, 0)]);
                }
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
            }
            QQC2.ComboBox {
                id: categoryChoice

                model: picker.categories
                Accessible.name: qsTr("Category")
            }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list

                model: picker.shown
                clip: true
                currentIndex: 0
                keyNavigationEnabled: true

                delegate: QQC2.ItemDelegate {
                    id: item

                    required property var modelData
                    required property int index
                    readonly property bool usable: modelData.problem.length === 0

                    width: ListView.view.width
                    highlighted: ListView.isCurrentItem
                    enabled: usable
                    onClicked: list.currentIndex = index
                    onDoubleClicked: picker.choose(modelData)

                    contentItem: ColumnLayout {
                        spacing: 0
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: item.modelData.name
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            font: Kirigami.Theme.smallFont
                            opacity: 0.7
                            elide: Text.ElideRight
                            text: item.usable
                                  ? [picker.format(item.modelData), item.modelData.category, item.modelData.author,
                                     qsTr("%1 in, %2 out").arg(item.modelData.audioIn).arg(item.modelData.audioOut)]
                                    .filter(s => s.length > 0).join(" · ")
                                  : qsTr("Can't be used: %1").arg(item.modelData.problem)
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: list.count === 0
                    icon.name: "plugins"
                    text: Devices.pluginsScanning ? qsTr("Looking for plugins…")
                          : Devices.plugins.length === 0 ? qsTr("No plugins installed")
                          : qsTr("No plugin matches")
                    explanation: Devices.plugins.length === 0 && !Devices.pluginsScanning
                                 ? qsTr("Install LV2, CLAP or VST3 plugins from your distribution, for example LSP, x42, Calf or ZAM plugins, then press Scan Plugins.")
                                 : ""
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            QQC2.CheckBox {
                id: showAll

                text: qsTr("Show plugins that can't be used")
            }
            Item {
                Layout.fillWidth: true
            }
            PluginScan {}
            QQC2.Button {
                icon.name: "list-add"
                text: qsTr("Add")
                enabled: list.currentIndex >= 0 && list.currentIndex < picker.shown.length
                         && picker.shown[list.currentIndex].problem.length === 0
                onClicked: picker.choose(picker.shown[list.currentIndex])
            }
        }
    }
}

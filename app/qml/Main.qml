// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    // "devices", "routing", "apps", "recordings" or "about" opens that page first, "guide"
    // the user guide (set from PATCHPRIME_PAGE).
    property string startPage
    // A strip label whose effects panel opens at start (set from PATCHPRIME_EFFECTS).
    property string openEffects
    // The user guide's Markdown (docs/HELP.md, built in).
    property string guideText

    readonly property string repository: "https://github.com/kingsqueak/patch-prime"

    title: Qt.application.displayName // alone: Qt adds it only to other titles
    width: Kirigami.Units.gridUnit * 52
    height: Kirigami.Units.gridUnit * 34

    // The size it was last left at (not maximized or full screen), in
    // ~/.config/kingsqueak.org/patchprime.conf; opened at that size again, but never larger
    // than the screen's free area.
    Settings {
        id: saved
        category: "MainWindow"
        property int width: 0
        property int height: 0
    }
    property bool sizeRestored: false
    function rememberSize() {
        if (sizeRestored && visibility === Window.Windowed) {
            saved.width = width;
            saved.height = height;
        }
    }
    onWidthChanged: rememberSize()
    onHeightChanged: rememberSize()

    // One page at a time, full width, with a back button. Side-by-side
    // columns put the wide mixer under the page next to it.
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn

    // Opens a page over the mixer, replacing any page already open.
    function open(page) {
        pageStack.pop(pageStack.initialPage);
        pageStack.push(page);
    }

    menuBar: QQC2.MenuBar {
        QQC2.Menu {
            title: qsTr("&File")

            QQC2.MenuItem {
                text: qsTr("&Quit")
                icon.name: "application-exit"
                action: QQC2.Action {
                    shortcut: StandardKey.Quit
                    onTriggered: Qt.quit()
                }
            }
        }

        QQC2.Menu {
            title: qsTr("&View")

            QQC2.MenuItem {
                text: qsTr("&Mixer")
                icon.name: "view-media-equalizer"
                action: QQC2.Action {
                    shortcut: "Ctrl+1"
                    onTriggered: root.pageStack.pop(root.pageStack.initialPage)
                }
            }
            QQC2.MenuItem {
                text: qsTr("&Apps")
                icon.name: "applications-multimedia"
                action: QQC2.Action {
                    shortcut: "Ctrl+2"
                    onTriggered: root.open(appsPage)
                }
            }
            QQC2.MenuItem {
                text: qsTr("&Routing")
                icon.name: "network-connect"
                action: QQC2.Action {
                    shortcut: "Ctrl+3"
                    onTriggered: root.open(routingPage)
                }
            }
            QQC2.MenuItem {
                text: qsTr("&Devices")
                icon.name: "audio-card"
                action: QQC2.Action {
                    shortcut: "Ctrl+4"
                    onTriggered: root.open(devicesPage)
                }
            }
            QQC2.MenuItem {
                text: qsTr("R&ecordings")
                icon.name: "media-record"
                action: QQC2.Action {
                    shortcut: "Ctrl+5"
                    onTriggered: root.open(recordingsPage)
                }
            }
            QQC2.MenuItem {
                text: qsTr("&Soundboard")
                icon.name: "media-playlist-play"
                action: QQC2.Action {
                    shortcut: "Ctrl+6"
                    onTriggered: root.open(soundboardPage)
                }
            }
        }

        QQC2.Menu {
            title: qsTr("&Help")

            QQC2.MenuItem {
                text: qsTr("Patch Prime &User Guide")
                icon.name: "help-contents"
                action: QQC2.Action {
                    shortcut: StandardKey.HelpContents
                    onTriggered: userGuide.show()
                }
            }
            QQC2.MenuItem {
                text: qsTr("User Guide &Online")
                icon.name: "internet-services"
                onTriggered: Qt.openUrlExternally(root.repository + "/blob/main/docs/HELP.md")
            }
            QQC2.MenuSeparator {}
            QQC2.MenuItem {
                text: qsTr("&Report a Bug…")
                icon.name: "tools-report-bug"
                onTriggered: Qt.openUrlExternally(root.repository + "/issues")
            }
            QQC2.MenuSeparator {}
            QQC2.MenuItem {
                text: qsTr("&About Patch Prime")
                icon.name: "help-about"
                onTriggered: root.open(aboutPage)
            }
        }
    }

    footer: MidiLearnBanner {}

    UserGuide {
        id: userGuide
        markdown: root.guideText
    }

    pageStack.initialPage: MixerPage {
        onOpenDevices: root.open(devicesPage)
        onOpenRouting: root.open(routingPage)
        onOpenApps: root.open(appsPage)
        onOpenRecordings: root.open(recordingsPage)
        onOpenSoundboard: root.open(soundboardPage)
    }

    Component {
        id: recordingsPage
        RecordingsPage {}
    }

    Component {
        id: soundboardPage
        SoundboardPage {}
    }

    Component {
        id: appsPage
        AppsPage {}
    }

    Component {
        id: devicesPage
        DevicesPage {}
    }

    Component {
        id: routingPage
        RoutingPage {}
    }

    Component {
        id: aboutPage

        Kirigami.AboutPage {
            aboutData: ({
                displayName: "Patch Prime",
                productName: "patchprime",
                componentName: "patchprime",
                desktopFileName: "org.kingsqueak.patchprime",
                programLogo: "view-media-equalizer",
                version: Qt.application.version,
                shortDescription: qsTr("A virtual audio mixer and router for KDE Plasma on PipeWire"),
                homepage: root.repository,
                bugAddress: root.repository + "/issues",
                copyrightStatement: "© 2026 KC2RGW Chris",
                otherText: qsTr("Noise suppression uses RNNoise (BSD-3-Clause), Copyright Xiph.Org Foundation and others."),
                authors: [{
                    name: "KC2RGW Chris",
                    task: qsTr("Author"),
                    emailAddress: "",
                    webAddress: "https://github.com/kingsqueak",
                    ocsUsername: ""
                }],
                credits: [],
                translators: [],
                licenses: [{
                    name: "GNU General Public License v3.0 or later",
                    text: qsTr("This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.\n\nThis program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details: https://www.gnu.org/licenses/gpl-3.0.html"),
                    spdx: "GPL-3.0-or-later"
                }]
            })

            // Below the authors (there are no credits or translators): a word
            // on supporting the work, in the page's own style.
            Kirigami.Heading {
                Layout.fillWidth: true
                Kirigami.FormData.isSection: true
                text: qsTr("Do You Find This Useful?")
                wrapMode: Text.WordWrap
            }
            QQC2.Label {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.gridUnit
                text: qsTr("I am unemployed and any support goes toward my expenses and equipment.")
                wrapMode: Text.WordWrap
            }
            Kirigami.UrlButton {
                Layout.leftMargin: Kirigami.Units.gridUnit
                url: "https://github.com/sponsors/kingsqueak/"
            }
        }
    }

    Component.onCompleted: {
        if (saved.width > 0 && saved.height > 0) {
            width = Math.min(Math.max(saved.width, minimumWidth), Screen.desktopAvailableWidth);
            height = Math.min(Math.max(saved.height, minimumHeight), Screen.desktopAvailableHeight);
        }
        sizeRestored = true;
        if (startPage === "devices")
            open(devicesPage);
        else if (startPage === "routing")
            open(routingPage);
        else if (startPage === "apps")
            open(appsPage);
        else if (startPage === "recordings")
            open(recordingsPage);
        else if (startPage === "soundboard")
            open(soundboardPage);
        else if (startPage === "about")
            open(aboutPage);
        else if (startPage === "guide")
            userGuide.show();
    }
}

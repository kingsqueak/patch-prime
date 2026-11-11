// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A newer release is out (see UpdateChecker): a note across the top of the
// window, with a link to the release. Closing it hides that version for good;
// the next one shows again.
Kirigami.InlineMessage {
    position: Kirigami.InlineMessage.Position.Header
    type: Kirigami.MessageType.Information
    icon.name: "system-software-update"
    visible: Updates.showNotice
    showCloseButton: true
    text: qsTr("Patch Prime %1 is out. You have %2.").arg(Updates.latest).arg(Updates.current)
    actions: [
        Kirigami.Action {
            text: qsTr("What's New")
            icon.name: "internet-services"
            onTriggered: Qt.openUrlExternally(Updates.page)
        }
    ]
    // The close button hides the message itself.
    onVisibleChanged: {
        if (!visible && Updates.showNotice)
            Updates.dismiss();
    }
}

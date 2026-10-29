// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Shown at the top of a page while the engine isn't usable.
Kirigami.InlineMessage {
    position: Kirigami.InlineMessage.Header
    visible: !Devices.daemonRunning || !Devices.connected
    type: Devices.daemonRunning ? Kirigami.MessageType.Warning : Kirigami.MessageType.Error
    text: !Devices.daemonRunning
          ? (Devices.lastError.length > 0 ? Devices.lastError
                                          : qsTr("The Patch Prime engine isn't running."))
          : qsTr("The engine isn't connected to PipeWire. Retrying…")
    actions: Kirigami.Action {
        visible: !Devices.daemonRunning
        text: qsTr("Start Engine")
        icon.name: "media-playback-start"
        onTriggered: Devices.startDaemon()
    }
}

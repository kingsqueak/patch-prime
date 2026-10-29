// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A strip's vertical fader in dB. `volumeDb` is the daemon's value;
// `requested(db)` asks for a new one. Double-click for 0 dB.
QQC2.Slider {
    id: fader

    property real volumeDb: 0
    property string label

    signal requested(real db)

    orientation: Qt.Vertical
    from: Devices.minimumVolumeDb
    to: Devices.maximumVolumeDb
    // Half-dB steps, rounded here: a stepSize makes Breeze draw ticks.
    onMoved: requested(Math.round(value * 2) / 2)
    Accessible.name: qsTr("Level for %1").arg(label)

    // Follow the daemon, except while the user is dragging.
    Binding on value {
        when: !fader.pressed
        value: fader.volumeDb
        restoreMode: Binding.RestoreNone
    }

    TapHandler {
        onDoubleTapped: fader.requested(0)
    }

    QQC2.ToolTip.text: qsTr("Double-click for 0 dB")
    QQC2.ToolTip.visible: hovered && !pressed
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
}

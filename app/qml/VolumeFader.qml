// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// A level slider in dB: a strip's fader (vertical) or a send level
// (horizontal). `volumeDb` is the daemon's value; `requested(db)` asks for a
// new one. Double-click for 0 dB.
QQC2.Slider {
    id: fader

    property real volumeDb: 0
    // For screen readers, e.g. "Level for Mic1".
    property string accessibleName

    signal requested(real db)

    orientation: Qt.Vertical
    from: Devices.minimumVolumeDb
    to: Devices.maximumVolumeDb
    // Half-dB steps, rounded here: a stepSize makes Breeze draw ticks.
    onMoved: requested(Math.round(value * 2) / 2)
    Accessible.name: accessibleName

    // Follow the daemon, except while the user is dragging. Assigned here
    // rather than with a Binding: a Binding that switched off while pressed
    // could leave the slider on an old value once the user had moved it, so a
    // typed level changed the sound but not the slider.
    onVolumeDbChanged: {
        if (!pressed)
            value = volumeDb;
    }
    onPressedChanged: {
        if (!pressed)
            value = volumeDb; // the daemon's echo of the drag, usually the same
    }
    Component.onCompleted: value = volumeDb

    TapHandler {
        onDoubleTapped: fader.requested(0)
    }

    QQC2.ToolTip.text: qsTr("Double-click for 0 dB")
    QQC2.ToolTip.visible: hovered && !pressed
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
}

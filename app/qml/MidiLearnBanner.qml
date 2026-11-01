// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

import QtQuick
import org.kde.kirigami as Kirigami
import org.kingsqueak.patchprime.engine

// Shown while the engine waits for a control to learn (see MidiLearnMenu).
Kirigami.InlineMessage {
    position: Kirigami.InlineMessage.Position.Footer
    type: Kirigami.MessageType.Information
    icon.name: "audio-midi"
    visible: Devices.midiLearning !== null && Devices.midiLearning !== undefined
    text: qsTr("MIDI Learn: move a fader or knob, or press a button, on your MIDI controller to control %1.")
          .arg(Devices.midiLearningText)
    actions: [
        Kirigami.Action {
            text: qsTr("Cancel")
            icon.name: "dialog-cancel"
            onTriggered: Devices.cancelMidiLearn()
        }
    ]
}

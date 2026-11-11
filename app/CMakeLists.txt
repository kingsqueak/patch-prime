# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS DBus Gui Network Qml Quick QuickControls2)
# Kirigami is used only from QML and is loaded at run time, so there's no
# find_package(KF6Kirigami): its CMake config needs OpenMP, which Clang builds lack.

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(patchprime main.cpp src/daemonclient.cpp src/daemonclient.h src/effectstrings.cpp src/effectstrings.h src/guideindex.cpp src/guideindex.h src/updatechecker.cpp src/updatechecker.h src/userguide.cpp src/userguide.h src/windowservice.cpp src/windowservice.h)
set_source_files_properties(qml/Style.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(patchprime
    URI org.kingsqueak.patchprime
    VERSION 1.0
    QML_FILES qml/Main.qml qml/MixerPage.qml qml/ChannelStrip.qml qml/DevicesPage.qml qml/EngineMessage.qml qml/RoutingPage.qml qml/AppsPage.qml qml/RecordingsPage.qml qml/RecordingsPanel.qml qml/RecordingsState.qml qml/RecordingRow.qml qml/PlayerControls.qml qml/TakeControls.qml qml/LoudnessPanel.qml
        qml/Style.qml qml/LevelMeter.qml qml/StripToggle.qml qml/VolumeFader.qml qml/StripGroup.qml qml/SendButton.qml qml/DbField.qml qml/SceneBar.qml qml/EffectsWindow.qml qml/InsertSlots.qml qml/ConsoleFader.qml
        qml/EqEditor.qml qml/ParamField.qml qml/MultibandEditor.qml qml/EffectParamRow.qml qml/PluginPicker.qml qml/PluginScan.qml
        qml/EffectMeterBridge.qml qml/GainReductionMeter.qml qml/effectmeters.js qml/UserGuide.qml
        qml/MidiLearnMenu.qml qml/MidiLearnBanner.qml qml/HotkeyDialog.qml qml/AppStrip.qml qml/AppStripGroup.qml qml/AppCompactRow.qml qml/PadButton.qml qml/PadDialog.qml qml/SoundboardPage.qml qml/SoundboardPanel.qml qml/PresetButton.qml qml/SetupWizard.qml qml/MiniRow.qml qml/MiniWindow.qml qml/UpdateBanner.qml
    NO_IMPORT_SCAN) # the import scan only matters for static Qt builds

# The user guide is built in (Help > User Guide), so it's always there and
# matches this version.
set_source_files_properties(${PROJECT_SOURCE_DIR}/docs/HELP.md PROPERTIES QT_EXCLUDE_FROM_TRANSLATION ON)
qt_add_resources(patchprime "guide" PREFIX "/" BASE "${PROJECT_SOURCE_DIR}"
    FILES "${PROJECT_SOURCE_DIR}/docs/HELP.md")

target_include_directories(patchprime PRIVATE src)
# The GUI talks to patchprimed over D-Bus and doesn't link the engine; only
# the plain DSP code, to draw effect curves.
target_link_libraries(patchprime PRIVATE
    Qt6::DBus Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 patchprime-dsp)
target_compile_definitions(patchprime PRIVATE QT_NO_CAST_FROM_ASCII
    PATCHPRIME_VERSION="${PROJECT_VERSION}")
patchprime_set_warnings(patchprime)
install(TARGETS patchprime)

# The launcher; its name matches QGuiApplication::desktopFileName().
configure_file(${PROJECT_SOURCE_DIR}/data/org.kingsqueak.patchprime.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.kingsqueak.patchprime.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kingsqueak.patchprime.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# The app's icon (PatchPrimeMac's), named as the launcher, in the hicolor theme:
# scalable, and as PNGs for desktops that don't draw SVG icons.
install(FILES ${PROJECT_SOURCE_DIR}/data/icons/org.kingsqueak.patchprime.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
foreach(size 48 64 128 256 512)
    install(FILES ${PROJECT_SOURCE_DIR}/data/icons/org.kingsqueak.patchprime-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME org.kingsqueak.patchprime.png)
endforeach()

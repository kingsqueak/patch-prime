# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS DBus Gui Qml Quick QuickControls2)
# Kirigami is used only from QML and is loaded at run time, so there's no
# find_package(KF6Kirigami): its CMake config needs OpenMP, which Clang builds lack.

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(patchprime main.cpp src/daemonclient.cpp src/daemonclient.h)
set_source_files_properties(qml/Style.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(patchprime
    URI org.kingsqueak.patchprime
    VERSION 1.0
    QML_FILES qml/Main.qml qml/MixerPage.qml qml/ChannelStrip.qml qml/DevicesPage.qml qml/EngineMessage.qml qml/RoutingPage.qml qml/AppsPage.qml
        qml/Style.qml qml/LevelMeter.qml qml/StripToggle.qml qml/VolumeFader.qml qml/StripGroup.qml qml/SendButton.qml qml/DbField.qml
    NO_IMPORT_SCAN) # the import scan only matters for static Qt builds

target_include_directories(patchprime PRIVATE src)
# The GUI talks to patchprimed over D-Bus and doesn't link the engine.
target_link_libraries(patchprime PRIVATE
    Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_compile_definitions(patchprime PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime)
install(TARGETS patchprime)

# The launcher; its name matches QGuiApplication::desktopFileName().
configure_file(${PROJECT_SOURCE_DIR}/data/org.kingsqueak.patchprime.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.kingsqueak.patchprime.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kingsqueak.patchprime.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

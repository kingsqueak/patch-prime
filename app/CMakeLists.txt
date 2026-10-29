# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
# Kirigami is used only from QML and is loaded at run time, so there's no
# find_package(KF6Kirigami): its CMake config needs OpenMP, which Clang builds lack.

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(patchprime main.cpp)
qt_add_qml_module(patchprime
    URI org.kingsqueak.patchprime
    VERSION 1.0
    QML_FILES qml/Main.qml qml/DevicesPage.qml
    NO_IMPORT_SCAN) # the import scan only matters for static Qt builds

target_link_libraries(patchprime PRIVATE
    PatchPrime::Engine
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_compile_definitions(patchprime PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime)
install(TARGETS patchprime)

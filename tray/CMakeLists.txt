# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS DBus Widgets)
# PATCHPRIME_REQUIRE_TRAY=ON (the package builds) makes it an error.
option(PATCHPRIME_REQUIRE_TRAY "Fail when KF6StatusNotifierItem is missing" OFF)
if(PATCHPRIME_REQUIRE_TRAY)
    find_package(KF6StatusNotifierItem 6.0 REQUIRED)
else()
    find_package(KF6StatusNotifierItem 6.0)
endif()
if(NOT KF6StatusNotifierItem_FOUND)
    message(WARNING "KF6StatusNotifierItem not found: building without the tray icon "
                    "(libkf6statusnotifieritem-dev, kf6-kstatusnotifieritem-devel, kstatusnotifieritem)")
    return()
endif()

add_executable(patchprime-tray main.cpp tray.cpp tray.h)
target_link_libraries(patchprime-tray PRIVATE Qt6::DBus Qt6::Widgets KF6::StatusNotifierItem)
target_compile_definitions(patchprime-tray PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime-tray)
install(TARGETS patchprime-tray)

# Started at login (it quits at once when the tray icon is off in the settings).
install(FILES ${PROJECT_SOURCE_DIR}/data/org.kingsqueak.patchprime.tray.desktop
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)

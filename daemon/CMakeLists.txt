# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS DBus)

add_executable(patchprimed main.cpp globalshortcuts.cpp globalshortcuts.h notifier.cpp notifier.h service.cpp service.h)
target_link_libraries(patchprimed PRIVATE PatchPrime::Engine Qt6::Core Qt6::DBus)
target_compile_definitions(patchprimed PRIVATE
    QT_NO_CAST_FROM_ASCII
    PATCHPRIME_VERSION="${PROJECT_VERSION}")
patchprime_set_warnings(patchprimed)
install(TARGETS patchprimed)

# systemd user unit and D-Bus activation, so the GUI can start the daemon.
configure_file(${PROJECT_SOURCE_DIR}/data/patchprimed.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/patchprimed.service @ONLY)
configure_file(${PROJECT_SOURCE_DIR}/data/org.kingsqueak.PatchPrime.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.kingsqueak.PatchPrime.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/patchprimed.service
        DESTINATION ${PATCHPRIME_SYSTEMD_USER_UNIT_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kingsqueak.PatchPrime.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

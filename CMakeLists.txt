# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

cmake_minimum_required(VERSION 3.28)

project(PatchPrime
    VERSION 0.0.1
    DESCRIPTION "Virtual audio mixer and router for KDE Plasma on PipeWire"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

option(PATCHPRIME_BUILD_APP "Build the Kirigami GUI" ON)
option(PATCHPRIME_BUILD_TESTS "Build the tests" ON)
option(PATCHPRIME_WERROR "Treat compiler warnings as errors" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

include(GNUInstallDirs)
include(cmake/Warnings.cmake)

# systemd also searches $XDG_DATA_DIRS/systemd/user, so this works for both a
# /usr and a ~/.local install.
set(PATCHPRIME_SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_DATADIR}/systemd/user"
    CACHE STRING "Where to install the patchprimed systemd user unit")

find_package(Qt6 6.10 REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.6)

add_subdirectory(engine)
add_subdirectory(daemon)
add_subdirectory(probe)

if(PATCHPRIME_BUILD_APP)
    add_subdirectory(app)
endif()

if(PATCHPRIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

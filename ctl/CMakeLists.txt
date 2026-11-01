# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS DBus)

# The planning (command -> D-Bus calls), apart so the tests can use it.
add_library(patchprime-ctl-plan STATIC ctl.cpp ctl.h)
target_include_directories(patchprime-ctl-plan PUBLIC .)
target_link_libraries(patchprime-ctl-plan PUBLIC Qt6::Core)
target_compile_definitions(patchprime-ctl-plan PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime-ctl-plan)

add_executable(patchprime-ctl main.cpp)
target_link_libraries(patchprime-ctl PRIVATE patchprime-ctl-plan Qt6::DBus)
target_compile_definitions(patchprime-ctl PRIVATE
    QT_NO_CAST_FROM_ASCII
    PATCHPRIME_VERSION="${PROJECT_VERSION}")
patchprime_set_warnings(patchprime-ctl)
install(TARGETS patchprime-ctl)

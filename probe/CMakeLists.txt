# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

add_executable(patchprime-probe main.cpp)
target_link_libraries(patchprime-probe PRIVATE PatchPrime::Engine Qt6::Core)
target_compile_definitions(patchprime-probe PRIVATE PATCHPRIME_VERSION="${PROJECT_VERSION}")
patchprime_set_warnings(patchprime-probe)
install(TARGETS patchprime-probe)

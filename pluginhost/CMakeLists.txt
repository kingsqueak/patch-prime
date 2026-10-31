# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# Runs plugins for the engine, one process per plugin (see PluginRunner),
# so all plugin code (lilv, the plugins themselves) stays out of the engine.
add_executable(patchprime-pluginhost lv2.cpp lv2.h main.cpp)
target_link_libraries(patchprime-pluginhost PRIVATE PatchPrime::Engine Qt6::Core PkgConfig::Lilv)
target_compile_definitions(patchprime-pluginhost PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime-pluginhost)
install(TARGETS patchprime-pluginhost DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

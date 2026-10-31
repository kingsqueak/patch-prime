# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# Runs plugins (LV2 and CLAP) for the engine, one process per plugin (see
# PluginRunner), so all plugin code (lilv, the plugins themselves) stays out
# of the engine.
# Plugin windows are plain X11 windows (the plugins' own X11 UIs inside).
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)

add_executable(patchprime-pluginhost
    clap.cpp clap.h eventring.h hostedplugin.h lv2.cpp lv2.h lv2ui.cpp lv2ui.h main.cpp
    x11window.cpp x11window.h)
target_link_libraries(patchprime-pluginhost PRIVATE
    PatchPrime::Engine Qt6::Core PkgConfig::Lilv PkgConfig::X11 patchprime-clap ${CMAKE_DL_LIBS})
target_compile_definitions(patchprime-pluginhost PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime-pluginhost)
install(TARGETS patchprime-pluginhost DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

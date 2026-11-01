# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

find_package(Qt6 6.10 REQUIRED COMPONENTS Test)

# Usage: patchprime_add_test(<name> <source>...)
function(patchprime_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE PatchPrime::Engine Qt6::Test)
    target_compile_definitions(${name} PRIVATE QT_NO_CAST_FROM_ASCII)
    patchprime_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

patchprime_add_test(test_defaultnodename test_defaultnodename.cpp)
patchprime_add_test(test_endpoint test_endpoint.cpp)
patchprime_add_test(test_devicestore test_devicestore.cpp)
patchprime_add_test(test_devicemodel test_devicemodel.cpp)
patchprime_add_test(test_busmanager test_busmanager.cpp)
patchprime_add_test(test_approuter test_approuter.cpp)
patchprime_add_test(test_audiosetup test_audiosetup.cpp)
patchprime_add_test(test_hotplugrecovery test_hotplugrecovery.cpp)
patchprime_add_test(test_scenes test_scenes.cpp)
patchprime_add_test(test_effects test_effects.cpp)
patchprime_add_test(test_recording test_recording.cpp)
patchprime_add_test(test_mackie test_mackie.cpp fakemidiport.h)
patchprime_add_test(test_midimapper test_midimapper.cpp fakemidiport.h)
patchprime_add_test(test_hotkeys test_hotkeys.cpp)
# The daemon's kglobalaccel backend, against the session's kglobalaccel.
find_package(Qt6 6.10 REQUIRED COMPONENTS DBus)
patchprime_add_test(test_globalshortcuts test_globalshortcuts.cpp ${PROJECT_SOURCE_DIR}/daemon/globalshortcuts.cpp
                    ${PROJECT_SOURCE_DIR}/daemon/globalshortcuts.h)
target_include_directories(test_globalshortcuts PRIVATE ${PROJECT_SOURCE_DIR}/daemon)
target_link_libraries(test_globalshortcuts PRIVATE Qt6::DBus)
target_link_libraries(test_mackie PRIVATE PkgConfig::Alsa) # its stand-in controller

# A tiny LV2 plugin (gain, and a switch that crashes it) for the plugin
# tests, which look for plugins only here.
add_library(patchprime-testgain MODULE lv2/testgain.c)
target_link_libraries(patchprime-testgain PRIVATE PkgConfig::Lilv)
set(PATCHPRIME_TEST_LV2 ${CMAKE_CURRENT_BINARY_DIR}/lv2)
set_target_properties(patchprime-testgain PROPERTIES
    PREFIX ""
    OUTPUT_NAME testgain
    LIBRARY_OUTPUT_DIRECTORY ${PATCHPRIME_TEST_LV2}/patchprime-test.lv2)
set(TEST_GAIN_BINARY testgain${CMAKE_SHARED_MODULE_SUFFIX})
configure_file(lv2/manifest.ttl.in ${PATCHPRIME_TEST_LV2}/patchprime-test.lv2/manifest.ttl @ONLY)
configure_file(lv2/testgain.ttl ${PATCHPRIME_TEST_LV2}/patchprime-test.lv2/testgain.ttl COPYONLY)

# The same as a CLAP plugin, alone in its search path.
add_library(patchprime-testgain-clap MODULE clap/testgain.c)
target_link_libraries(patchprime-testgain-clap PRIVATE patchprime-clap)
set(PATCHPRIME_TEST_CLAP ${CMAKE_CURRENT_BINARY_DIR}/clap)
set_target_properties(patchprime-testgain-clap PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME testgain
    LIBRARY_OUTPUT_DIRECTORY ${PATCHPRIME_TEST_CLAP})
# And as a VST3 bundle, alone in its search path.
add_library(patchprime-testgain-vst3 MODULE vst3/testgain.cpp)
target_link_libraries(patchprime-testgain-vst3 PRIVATE patchprime-vst3)
set(PATCHPRIME_TEST_VST3 ${CMAKE_CURRENT_BINARY_DIR}/vst3)
set_target_properties(patchprime-testgain-vst3 PROPERTIES
    PREFIX ""
    OUTPUT_NAME patchprime-test
    CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY
        ${PATCHPRIME_TEST_VST3}/patchprime-test.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux)

set(PATCHPRIME_TEST_PLUGIN_ENV
    "LV2_PATH=${PATCHPRIME_TEST_LV2};PATCHPRIME_CLAP_PATH=${PATCHPRIME_TEST_CLAP};PATCHPRIME_VST3_PATH=${PATCHPRIME_TEST_VST3}")

patchprime_add_test(test_plugins test_plugins.cpp)
add_dependencies(test_plugins patchprime-testgain patchprime-testgain-clap patchprime-testgain-vst3
                 patchprime-pluginhost)
set_tests_properties(test_plugins PROPERTIES ENVIRONMENT "${PATCHPRIME_TEST_PLUGIN_ENV}")

# Needs a running PipeWire server; skips itself when there isn't one.
patchprime_add_test(test_graphsnapshot test_graphsnapshot.cpp)
set_tests_properties(test_graphsnapshot PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

# Creates and removes a temporary null sink with pactl; skips without PipeWire.
patchprime_add_test(test_pipewiregraph test_pipewiregraph.cpp)
set_tests_properties(test_pipewiregraph PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

# Creates a null sink, enables it, and checks its bus, links, volume and meter.
patchprime_add_test(test_buslive test_buslive.cpp)
add_dependencies(test_buslive patchprime-testgain patchprime-testgain-clap patchprime-testgain-vst3
                 patchprime-pluginhost)
set_tests_properties(test_buslive PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77
    ENVIRONMENT "${PATCHPRIME_TEST_PLUGIN_ENV}")

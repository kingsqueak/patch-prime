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

patchprime_add_test(test_plugins test_plugins.cpp)
add_dependencies(test_plugins patchprime-testgain patchprime-pluginhost)
set_tests_properties(test_plugins PROPERTIES ENVIRONMENT "LV2_PATH=${PATCHPRIME_TEST_LV2}")

# Needs a running PipeWire server; skips itself when there isn't one.
patchprime_add_test(test_graphsnapshot test_graphsnapshot.cpp)
set_tests_properties(test_graphsnapshot PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

# Creates and removes a temporary null sink with pactl; skips without PipeWire.
patchprime_add_test(test_pipewiregraph test_pipewiregraph.cpp)
set_tests_properties(test_pipewiregraph PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

# Creates a null sink, enables it, and checks its bus, links, volume and meter.
patchprime_add_test(test_buslive test_buslive.cpp)
add_dependencies(test_buslive patchprime-testgain patchprime-pluginhost)
set_tests_properties(test_buslive PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77
    ENVIRONMENT "LV2_PATH=${PATCHPRIME_TEST_LV2}")

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

# Needs a running PipeWire server; skips itself when there isn't one.
patchprime_add_test(test_graphsnapshot test_graphsnapshot.cpp)
set_tests_properties(test_graphsnapshot PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

# Creates and removes a temporary null sink with pactl; skips without PipeWire.
patchprime_add_test(test_pipewiregraph test_pipewiregraph.cpp)
set_tests_properties(test_pipewiregraph PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

# Creates a null sink, enables it, and checks its bus, links, volume and meter.
patchprime_add_test(test_buslive test_buslive.cpp)
set_tests_properties(test_buslive PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

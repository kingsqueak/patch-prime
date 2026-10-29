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

# Needs a running PipeWire server; skips itself when there isn't one.
patchprime_add_test(test_graphsnapshot test_graphsnapshot.cpp)
set_tests_properties(test_graphsnapshot PROPERTIES LABELS "pipewire" SKIP_RETURN_CODE 77)

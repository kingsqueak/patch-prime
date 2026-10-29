# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

add_library(patchprime-engine STATIC
    src/busmanager.cpp
    src/busmanager.h
    src/devicemodel.cpp
    src/devicemodel.h
    src/devicestore.cpp
    src/devicestore.h
    src/endpoint.cpp
    src/endpoint.h
    src/graphsnapshot.cpp
    src/graphsnapshot.h
    src/graphtypes.h
    src/pipewiregraph.cpp
    src/pipewiregraph.h)
add_library(PatchPrime::Engine ALIAS patchprime-engine)

target_include_directories(patchprime-engine PUBLIC src)
target_link_libraries(patchprime-engine
    PUBLIC Qt6::Core
    PRIVATE PkgConfig::PipeWire)
target_compile_definitions(patchprime-engine PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime-engine)

# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# The built-in effects: plain C++ (no Qt, no PipeWire), shared by the engine
# (which runs them) and the GUI (which draws their curves).
add_library(patchprime-dsp STATIC
    src/dynamics.cpp
    src/dynamics.h
    src/effects.cpp
    src/effects.h
    src/eq.cpp
    src/eq.h
    src/multiband.cpp
    src/multiband.h)
target_include_directories(patchprime-dsp PUBLIC src)
if(PATCHPRIME_RNNOISE)
    target_sources(patchprime-dsp PRIVATE src/denoise.cpp src/denoise.h)
    target_link_libraries(patchprime-dsp PRIVATE patchprime-rnnoise)
    target_compile_definitions(patchprime-dsp PUBLIC PATCHPRIME_HAVE_RNNOISE)
endif()
patchprime_set_warnings(patchprime-dsp)

add_library(patchprime-engine STATIC
    src/approuter.cpp
    src/approuter.h
    src/appstore.cpp
    src/appstore.h
    src/audiosetup.cpp
    src/audiosetup.h
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
    src/hotplugrecovery.cpp
    src/hotplugrecovery.h
    src/pipewiregraph.cpp
    src/pipewiregraph.h
    src/scenes.cpp
    src/scenes.h)
add_library(PatchPrime::Engine ALIAS patchprime-engine)

target_include_directories(patchprime-engine PUBLIC src)
target_link_libraries(patchprime-engine
    PUBLIC Qt6::Core patchprime-dsp
    PRIVATE PkgConfig::PipeWire)
target_compile_definitions(patchprime-engine PRIVATE QT_NO_CAST_FROM_ASCII)
patchprime_set_warnings(patchprime-engine)

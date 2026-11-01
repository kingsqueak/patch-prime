# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# The VST 3 plugin interfaces (steinbergmedia/vst3_pluginterfaces, MIT since
# VST 3.8, compatible with the GPL), for hosting VST3 plugins. Only the
# interfaces: the host classes are our own (pluginhost/vst3.cpp). Fetched at
# configure time, pinned by its checksum.

include(FetchContent)
FetchContent_Declare(vst3pluginterfaces
    URL https://github.com/steinbergmedia/vst3_pluginterfaces/archive/refs/tags/v3.8.0_build_66.tar.gz
    URL_HASH SHA256=c922f0a35f2c093dfcb24ccd95282e70d00cc2de42fc0c8ac1577398ffe7b6d4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR headers-only) # no such directory: only unpacked, not built
FetchContent_MakeAvailable(vst3pluginterfaces)

# Its sources include "pluginterfaces/...": give them that directory name.
set(PATCHPRIME_VST3_INCLUDE ${CMAKE_BINARY_DIR}/vst3-include)
file(MAKE_DIRECTORY ${PATCHPRIME_VST3_INCLUDE})
file(CREATE_LINK ${vst3pluginterfaces_SOURCE_DIR} ${PATCHPRIME_VST3_INCLUDE}/pluginterfaces SYMBOLIC)

add_library(patchprime-vst3 STATIC
    ${vst3pluginterfaces_SOURCE_DIR}/base/coreiids.cpp
    ${vst3pluginterfaces_SOURCE_DIR}/base/funknown.cpp
    ${vst3pluginterfaces_SOURCE_DIR}/base/ustring.cpp
    ${PROJECT_SOURCE_DIR}/pluginhost/vst3iids.cpp)
target_include_directories(patchprime-vst3 SYSTEM PUBLIC ${PATCHPRIME_VST3_INCLUDE})
# Third-party code (and our list of its interface ids): its warnings, and
# clang-tidy's, aren't ours to fix.
target_compile_options(patchprime-vst3 PRIVATE -w)
set_target_properties(patchprime-vst3 PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_CLANG_TIDY "")

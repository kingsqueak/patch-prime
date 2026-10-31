# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# RNNoise (Xiph.Org, BSD-3-Clause, compatible with the GPL) for the noise
# suppression effect. Not packaged for Ubuntu, so the release is fetched at
# configure time, pinned by its checksum, and built as a small static library.
# The release tarball includes the trained model (src/rnnoise_data.c).

include(FetchContent)
FetchContent_Declare(rnnoise
    URL https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
    URL_HASH SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rnnoise) # no CMakeLists.txt of its own: only unpacked

add_library(patchprime-rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c
    ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_data.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c)
target_include_directories(patchprime-rnnoise SYSTEM PUBLIC ${rnnoise_SOURCE_DIR}/include)
target_include_directories(patchprime-rnnoise PRIVATE ${rnnoise_SOURCE_DIR}/src
    # os_support.h, which the release lacks but its ARM NEON code includes.
    ${CMAKE_CURRENT_LIST_DIR}/rnnoise)
# Third-party code: optimised even in debug builds (it runs on the realtime
# thread), and its warnings aren't ours to fix.
target_compile_options(patchprime-rnnoise PRIVATE -O2 -w)
set_target_properties(patchprime-rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(patchprime-rnnoise PRIVATE m)

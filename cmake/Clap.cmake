# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# The CLAP plugin API (free-audio/clap, MIT, compatible with the GPL), for
# hosting CLAP plugins. Header-only and not packaged for Ubuntu, so the
# release is fetched at configure time, pinned by its checksum.

include(FetchContent)
FetchContent_Declare(clap
    URL https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz
    URL_HASH SHA256=58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR headers-only) # no such directory: only unpacked, not built
FetchContent_MakeAvailable(clap)

add_library(patchprime-clap INTERFACE)
target_include_directories(patchprime-clap SYSTEM INTERFACE ${clap_SOURCE_DIR}/include)

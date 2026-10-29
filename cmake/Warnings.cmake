# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# Usage: patchprime_set_warnings(<target>)
function(patchprime_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
        -Wimplicit-fallthrough)
    # No -Wnull-dereference: it only runs when optimizing, and then flags
    # inlined code in PipeWire's and Qt's headers (false positives).
    # clang-tidy checks ours.
    if(PATCHPRIME_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# Usage: patchprime_set_warnings(<target>)
function(patchprime_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
        -Wnull-dereference -Wimplicit-fallthrough)
    if(PATCHPRIME_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

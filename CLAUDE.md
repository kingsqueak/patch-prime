# Patch Prime

A virtual audio mixer and router for KDE Plasma on PipeWire (Linux). It's inspired by Ginger Audio CASTER on macOS.

## Read first
- Plan and phases: `~/obsidian/Projects/Patch Prime/Patch Prime - Plan.md`
- Architecture: `~/obsidian/Projects/Patch Prime/Patch Prime - Architecture.md`
- Session log: `~/obsidian/Projects/Patch Prime/Patch Prime - Session Log.md` (add an entry at the end of each session)

## Stack
C++23 (for `std::expected` and `std::print`), CMake + Ninja, Qt 6.10 (QML + Kirigami), KF6 6.24, libpipewire-0.3 (PipeWire 1.6, WirePlumber 0.5). Ubuntu 26.04, Plasma 6.6.

## Ground rules
- The engine runs in a daemon (`patchprimed`, a systemd user service). The GUI is a D-Bus client and contains no realtime audio code.
- Never save PipeWire numeric IDs. Key everything by `media.class` + `node.name` / `device.name`.
- Every node Patch Prime creates is named `patchprime.*` and has `node.autoconnect=false`.
- Leave `node.description` and the hardware device volume alone. Labels and levels belong to the app.
- License: GPL-3.0-or-later. Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later` and `// SPDX-FileCopyrightText: 2026 KC2RGW Chris`. Third-party code must be GPL-3-compatible.
- Build with GCC and Clang. Delegate self-contained tasks (reviews, research, boilerplate) to Grok: `grok -p "..." --always-approve`.

## Build
- `cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug` (also `clang-debug`; `clang-tidy` runs clang-tidy during the build). The presets turn on `-Werror`.
- Layout: `engine/` (static library, Qt Core only, owns all libpipewire code), `probe/` (`patchprime-probe`), `app/` (Kirigami GUI), `tests/` (Qt Test; `test_graphsnapshot` needs a running PipeWire server and skips without one).
- Kirigami is used only from QML. Don't `find_package(KF6Kirigami)`: its CMake config needs OpenMP, which Clang builds here lack.

## GitHub
`Kingsqueak/patch-prime` over SSH.
- Solo project: commit straight to `main`, with no feature branches or PRs, until the app is usable end to end. Every commit must build warning-free with GCC and Clang and pass the tests. Push after each working step.

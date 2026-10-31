# Patch Prime

A virtual audio mixer and router for KDE Plasma on PipeWire (Linux).

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
- Processes: `patchprimed` (`daemon/`) hosts the engine: `PipeWireGraph` (pw_thread_loop, events posted to the main thread) → `DeviceModel` (+ `DeviceStore`, `~/.config/patchprime/devices.json`) → `BusManager` (one lingering `patchprime.bus.<in|out>.<busId>` null sink per enabled device, linked to its hardware; fader/mute = the bus's channelVolumes/mute). D-Bus: `org.kingsqueak.PatchPrime` `/org/kingsqueak/PatchPrime` `org.kingsqueak.PatchPrime1` (see `daemon/service.h`). The GUI (`app/`) is only a D-Bus client (`DaemonClient`) and doesn't link the engine.
- Audio setup: `AudioSetup` applies the mixer rate/block size to PipeWire's `settings` metadata, and writes per-device rules (scheduling, own rate) to `~/.config/wireplumber/wireplumber.conf.d/60-patchprime.conf`, restarting WirePlumber when that file changes. Pro-audio interfaces that follow another clock need timer scheduling, or they xrun and pulse.
- Installed (2026-10-29) as a systemd user service from `~/.local` (`systemctl --user status patchprimed`), plus a `.desktop` launcher. Configure with `-DCMAKE_INSTALL_PREFIX=$HOME/.local` (the unit, D-Bus and `.desktop` files hold full paths), then `cmake --install build/gcc-release && systemctl --user restart patchprimed`.
- Dev loop: only one engine can own the D-Bus name, so `systemctl --user stop patchprimed` first, then run `build/gcc-debug/daemon/patchprimed` in a terminal and the app; `systemctl --user start patchprimed` afterwards. `patchprimed --remove-buses` removes every bus. The tests run their own engines next to the service (separate owner tags and app scope).
- Don't use `pkill -f` with a pattern that appears in your own command line: it kills the shell running it. Use `pgrep -x patchprimed` / `pkill -x patchprimed`.
- QML delegates: read roles through `required property var model` (`model.foo`), not one required property per role, and don't use `ListView.section`. With Qt 6.10 both showed stale data on the wrong row after a row was inserted. Never name a role after an existing Item property (`enabled`, `visible`, ...). For a control the user drags (sliders), follow the daemon by assigning `value` in a change handler (see `VolumeFader.qml`), not with `Binding { when: !pressed }`: that left the slider drawn at one value and grabbed at another.
- `PATCHPRIME_SCREENSHOT=<file.png> QT_QPA_PLATFORM=offscreen` saves the window and quits, to check the UI without a display; point `XDG_CONFIG_HOME` at a temp dir so your real settings aren't touched (copy `~/.config/kdeglobals` there to render in the user's theme, or a `/usr/share/color-schemes/*.colors` file as `kdeglobals` for another). `PATCHPRIME_WINDOW_SIZE=1500x860` sizes the window, `PATCHPRIME_SCREENSHOT_DELAY=<ms>` waits longer (meters settle). `PATCHPRIME_PAGE=devices|routing|apps|about|guide` opens a page (or the user guide's window, saved as `<file>-2.png`); add `QT_QPA_PLATFORMTHEME=kde` for the Plasma palette (offscreen without it, the guide's Markdown links are dark blue). The user guide is compiled into the app (Help menu, F1). The README screenshot is `docs/images/mixer.png`; the user guide is `docs/HELP.md` (keep it in step with the app).
- Effects: `engine/src/` `effects`, `eq`, `multiband`, `dynamics`, `denoise` form `patchprime-dsp` (plain C++, linked by the engine and the GUI, which draws curves with it). RNNoise is fetched at configure time (`cmake/RNNoise.cmake`, pinned by SHA-256; `-DPATCHPRIME_RNNOISE=OFF` builds without noise suppression). `PATCHPRIME_EFFECTS=<strip label>` opens a strip's effects window for screenshots (saved as `<file>-2.png`).
- QML: never give an id or property an existing Item/Layout property name (`scale`, `spacing`, ...): it is shadowed or "set multiple times".
- Kirigami is used only from QML. Don't `find_package(KF6Kirigami)`: its CMake config needs OpenMP, which Clang builds here lack.

## GitHub
`Kingsqueak/patch-prime` over SSH.
- Solo project: commit straight to `main`, with no feature branches or PRs, until the app is usable end to end. Every commit must build warning-free with GCC and Clang and pass the tests. Push after each working step.

# Patch Prime

A virtual audio mixer and router for KDE Plasma, built on PipeWire.

- Finds the audio input and output devices on your system
- A device matrix where you choose which devices the mixer uses
- A channel strip for each device, with a label you choose, a fader, mute and a meter
- Send any input to any number of hardware or virtual outputs
- Create virtual devices (for example "Stream Mix" for OBS or Discord)
- Set the default system input and output

Planned: built-in EQ and dynamics processing, then LV2, CLAP and VST3 plugin hosting.

Status: early development. Phase 1: the app lists every audio input and output live (hotplug included), and lets you enable each one and give it a label. Settings are kept in `~/.config/patchprime/devices.json` and follow a device when it moves to another USB port or changes profile.

## Building

Ubuntu 26.04 / Plasma 6 build dependencies:

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-declarative-dev \
    libpipewire-0.3-dev libspa-0.2-dev libkirigami-dev kirigami-addons-dev
```

```sh
cmake --preset gcc-debug          # or clang-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

## patchprime-probe

Connects to PipeWire once and prints the graph: nodes, devices, ports, links and the default devices.

```sh
build/gcc-debug/probe/patchprime-probe            # JSON
build/gcc-debug/probe/patchprime-probe --summary  # short device list
```

A card whose profile is `off` (nothing plugged into its jacks, or no monitor on HDMI) has no nodes; `--summary` still lists it under *Cards*.

## License

Copyright © 2026 KC2RGW Chris

Patch Prime is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE).

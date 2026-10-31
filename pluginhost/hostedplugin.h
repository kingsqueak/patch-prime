// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "effects.h"

#include <functional>
#include <memory>
#include <string>

namespace pp {

// A plugin's own editor in a window of its own, on the host's main thread.
class PluginWindow
{
public:
    PluginWindow() = default;
    virtual ~PluginWindow() = default;
    PluginWindow(const PluginWindow &) = delete;
    PluginWindow &operator=(const PluginWindow &) = delete;
    PluginWindow(PluginWindow &&) = delete;
    PluginWindow &operator=(PluginWindow &&) = delete;

    // Why there's no window ("" when it's open).
    [[nodiscard]] virtual const std::string &error() const = 0;
    // Handles its events and brings it up to date; call it often (about 30
    // times a second). False once it has been closed.
    virtual bool idle() = 0;
    // Brings it to the front.
    virtual void raise() = 0;

    // A parameter the editor changed: its name and value.
    std::function<void(const std::string &, double)> onParam;
};

// A plugin (LV2 or CLAP) as an effect in the host's chain, with what the
// host's main loop needs of it apart from the audio.
class HostedPlugin : public Effect
{
public:
    // Why the plugin couldn't be started, or "" when it runs.
    [[nodiscard]] virtual const std::string &error() const = 0;
    // Its own state apart from its parameters ("" when it has none); set
    // the state to restore before prepare().
    [[nodiscard]] virtual std::string saveState() const = 0;
    virtual void setInitialState(std::string state) = 0;
    // Whether its state changed since the last call (a plugin that says so;
    // others are asked from time to time while their window is open).
    virtual bool takeStateDirty() { return false; }
    // Its editor (check the window's error()).
    virtual std::unique_ptr<PluginWindow> openWindow(const std::string &title) = 0;
    // Parameters the plugin changed by itself (its editor, in CLAP), as
    // fn(name, value), on the main thread.
    virtual void takeParamChanges(const std::function<void(const std::string &, double)> & /*fn*/) {}
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "hostedplugin.h"
#include "plugins.h"

#include <memory>
#include <string>
#include <vector>

namespace pp {

// VST3 plugins: where they're installed, and what each bundle holds.

// The .vst3 bundles in the search path (~/.vst3, /usr/lib/vst3,
// /usr/local/lib/vst3), or only $PATCHPRIME_VST3_PATH when it's set (the
// tests' own plugin).
std::vector<std::string> vst3Bundles();
// The effects in one bundle. Each is created to read its parameters and
// buses, so a crashing plugin crashes the caller: run it in a process of its
// own (patchprime-pluginhost --scan-vst3 <bundle>).
std::vector<PluginInfo> scanVst3Bundle(const std::string &path);
// A category for a VST3 class's sub-categories ("Fx|Dynamics" -> "Dynamics").
std::string vst3Category(const std::string &subCategories);

// A VST3 plugin as an effect in a chain (see EffectChain): its component
// processes, its edit controller (a separate object, or the component
// itself) holds the parameters and makes the editor.
//
// Parameters are named by ParamID, in decimal. A stepped one (a choice or a
// switch) takes its step's index; any other its normalized value, 0 to 1.
// They reach the processor as parameter changes and the controller through
// setParamNormalized; edits in the plugin's own editor come back through
// takeParamChanges, for the engine to save.
class Vst3Effect : public HostedPlugin
{
public:
    // Loads the effect with this class id (hex) from the bundle at path
    // (see error()).
    Vst3Effect(const std::string &path, const std::string &classId);
    ~Vst3Effect() override;
    Vst3Effect(const Vst3Effect &) = delete;
    Vst3Effect &operator=(const Vst3Effect &) = delete;
    Vst3Effect(Vst3Effect &&) = delete;
    Vst3Effect &operator=(Vst3Effect &&) = delete;

    [[nodiscard]] std::string_view type() const override;
    void prepare(double sampleRate, int channels) override;
    void process(float *const *channels, int channelCount, int frames) noexcept override;
    void setParam(const std::string &name, double value) override;
    void resume() noexcept override;

    [[nodiscard]] const std::string &error() const override;
    [[nodiscard]] std::string saveState() const override;
    void setInitialState(std::string state) override;
    bool takeStateDirty() override;
    std::unique_ptr<PluginWindow>
    openWindow(const std::string &title, std::function<void(const std::string &, double)> onParam) override;
    void takeParamChanges(const std::function<void(const std::string &, double)> &fn) override;
    void mainThreadTick() override;
    [[nodiscard]] bool wantsRestart() const override;

    struct Impl;

private:
    void processActive(float *const *channels, int channelCount, int frames) noexcept;

    std::unique_ptr<Impl> d;
};

} // namespace pp

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <lilv/lilv.h>
#include <lv2/data-access/data-access.h>
#include <lv2/options/options.h>
#include <lv2/ui/ui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Xlib's types, without its macros in every file that includes this.
using XDisplay = struct _XDisplay;
using XWindowId = unsigned long; // NOLINT(google-runtime-int): Xlib's Window

namespace pp {

class Lv2Effect;
class Lv2World;

// A plugin's X11 UI (ui:X11UI) that Patch Prime can show: what it is, and
// where its library is. nullopt when the plugin has none, or only ones
// needing what we don't provide (Gtk or Qt UIs, unknown required features).
struct UiChoice
{
    std::string uri;
    std::string binary; // the library's path
    std::string bundle; // its bundle directory, with a trailing slash
};
std::optional<UiChoice> findX11Ui(LilvWorld *world, const LilvPlugin *plugin);

// Makes Xlib safe for plugin UIs that use it from more than one thread; call
// first, before anything else touches X.
void initX11Threads();

// A plugin's own editor, in a window of its own (a plain X11 window the UI
// is embedded in). Lives on the host's main thread: idle() runs it.
//
// It talks to the plugin as an LV2 host does: control values both ways
// (what the UI changes goes to the plugin, and to onParam so the engine
// saves it; what changes elsewhere, and the plugin's meters, go to the UI),
// and atom events both ways (see Lv2Effect::sendAtom and takeEvents).
class Lv2Ui
{
public:
    Lv2Ui(Lv2World &world, Lv2Effect &effect, const std::string &title);
    ~Lv2Ui();
    Lv2Ui(const Lv2Ui &) = delete;
    Lv2Ui &operator=(const Lv2Ui &) = delete;
    Lv2Ui(Lv2Ui &&) = delete;
    Lv2Ui &operator=(Lv2Ui &&) = delete;

    // Why there's no window ("" when it's open).
    [[nodiscard]] const std::string &error() const { return m_error; }
    // Handles the window's events, runs the UI, and brings it up to date;
    // call it often (about 30 times a second). False once the window has
    // been closed (by its close button, or the UI itself).
    bool idle();
    // Brings the window to the front.
    void raise();

    // A control input the UI changed: its symbol and value.
    std::function<void(const std::string &, float)> onParam;

private:
    static void write(LV2UI_Controller controller, std::uint32_t port, std::uint32_t size,
                      std::uint32_t protocol, const void *buffer);
    static int resize(LV2UI_Feature_Handle handle, int width, int height);
    void close();

    Lv2Effect &m_effect;
    std::string m_error;
    XDisplay *m_display = nullptr;
    XWindowId m_window = 0;
    XWindowId m_child = 0;
    unsigned long m_deleteAtom = 0; // NOLINT(google-runtime-int): an X Atom
    int m_width = 0;
    int m_height = 0;
    const LV2UI_Descriptor *m_descriptor = nullptr;
    LV2UI_Handle m_handle = nullptr;
    const LV2UI_Idle_Interface *m_idle = nullptr;
    const LV2UI_Resize *m_uiResize = nullptr;
    std::uint32_t m_eventTransfer = 0;
    // What the UI was last told, by port index (NaN: not yet).
    std::vector<float> m_sent;
    // Features, which must outlive the UI.
    float m_rate = 48000.0F;
    float m_updateRate = 30.0F;
    std::vector<LV2_Options_Option> m_options;
    LV2UI_Resize m_resize{};
    LV2_Extension_Data_Feature m_dataAccess{};
    LV2_Feature m_parentFeature{};
    LV2_Feature m_resizeFeature{};
    LV2_Feature m_idleFeature{};
    LV2_Feature m_instanceFeature{};
    LV2_Feature m_dataFeature{};
    std::vector<const LV2_Feature *> m_features;
};

} // namespace pp

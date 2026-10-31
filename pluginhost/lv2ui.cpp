// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "lv2ui.h"

#include "lv2.h"

#include <lv2/atom/atom.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/parameters/parameters.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dlfcn.h>

#include <cstdio>
#include <print>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <utility>

namespace pp {

namespace {

// What a UI may require of us.
const std::set<std::string> &supportedUiFeatures()
{
    static const std::set<std::string> features{
        LV2_URID__map,     LV2_URID__unmap,           LV2_UI__parent,
        LV2_UI__resize,    LV2_UI__idleInterface,     LV2_UI__noUserResize,
        LV2_UI__fixedSize, LV2_INSTANCE_ACCESS_URI,   LV2_DATA_ACCESS_URI,
        LV2_OPTIONS__options, LV2_LOG__log,
    };
    return features;
}

std::string pathOf(const LilvNode *uri)
{
    if (uri == nullptr) {
        return {};
    }
    char *path = lilv_file_uri_parse(lilv_node_as_uri(uri), nullptr);
    std::string out = path != nullptr ? std::string(path) : std::string();
    lilv_free(path);
    return out;
}

// X errors a plugin's window causes (one already gone, say) aren't fatal.
int ignoreXError(Display * /*display*/, XErrorEvent *event)
{
    std::println(stderr, "patchprime-pluginhost: X error {} (request {})", event->error_code,
                 event->request_code);
    return 0;
}

} // namespace

std::optional<UiChoice> findX11Ui(LilvWorld *world, const LilvPlugin *plugin)
{
    LilvNode *x11 = lilv_new_uri(world, LV2_UI__X11UI);
    LilvNode *required = lilv_new_uri(world, LV2_CORE__requiredFeature);
    std::optional<UiChoice> choice;
    LilvUIs *uis = lilv_plugin_get_uis(plugin);
    LILV_FOREACH (uis, i, uis) {
        const LilvUI *ui = lilv_uis_get(uis, i);
        if (!lilv_ui_is_a(ui, x11)) {
            continue;
        }
        bool usable = true;
        LilvNodes *needs = lilv_world_find_nodes(world, lilv_ui_get_uri(ui), required, nullptr);
        LILV_FOREACH (nodes, n, needs) {
            const LilvNode *feature = lilv_nodes_get(needs, n);
            usable = usable && lilv_node_is_uri(feature) &&
                     supportedUiFeatures().contains(lilv_node_as_uri(feature));
        }
        lilv_nodes_free(needs);
        UiChoice c{.uri = lilv_node_as_uri(lilv_ui_get_uri(ui)),
                   .binary = pathOf(lilv_ui_get_binary_uri(ui)),
                   .bundle = pathOf(lilv_ui_get_bundle_uri(ui)),};
        if (usable && !c.binary.empty()) {
            if (!c.bundle.ends_with('/')) {
                c.bundle += '/';
            }
            choice = std::move(c);
            break;
        }
    }
    lilv_uis_free(uis);
    lilv_node_free(required);
    lilv_node_free(x11);
    return choice;
}

void initX11Threads()
{
    XInitThreads();
}

Lv2Ui::Lv2Ui(Lv2World &world, Lv2Effect &effect, const std::string &title)
    : m_effect(effect)
{
    const auto choice = findX11Ui(world.world(), effect.plugin());
    if (!choice) {
        m_error = "this plugin has no window Patch Prime can show";
        return;
    }
    if (effect.handle() == nullptr) {
        m_error = "the plugin isn't running";
        return;
    }
    m_display = XOpenDisplay(nullptr);
    if (m_display == nullptr) {
        m_error = "there's no display to show it on";
        return;
    }
    XSetErrorHandler(ignoreXError);

    // The window the UI goes in.
    const int screen = DefaultScreen(m_display);
    m_width = 640;
    m_height = 480;
    m_window = XCreateSimpleWindow(m_display, RootWindow(m_display, screen), 0, 0,
                                   static_cast<unsigned>(m_width), static_cast<unsigned>(m_height),
                                   0, BlackPixel(m_display, screen), BlackPixel(m_display, screen));
    XStoreName(m_display, m_window, title.c_str());
    XChangeProperty(m_display, m_window, XInternAtom(m_display, "_NET_WM_NAME", False),
                    XInternAtom(m_display, "UTF8_STRING", False), 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(title.c_str()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                    static_cast<int>(title.size()));
    std::string name = "patchprime";
    std::string className = "Patch Prime";
    XClassHint hint{.res_name = name.data(), .res_class = className.data()};
    XSetClassHint(m_display, m_window, &hint);
    m_deleteAtom = XInternAtom(m_display, "WM_DELETE_WINDOW", False);
    Atom protocols = m_deleteAtom;
    XSetWMProtocols(m_display, m_window, &protocols, 1);
    XSelectInput(m_display, m_window, StructureNotifyMask);

    // Its features.
    m_eventTransfer = world.map(LV2_ATOM__eventTransfer);
    m_rate = effect.sampleRate();
    m_options = {
        {LV2_OPTIONS_INSTANCE, 0, world.map(LV2_PARAMETERS__sampleRate), sizeof(float),
         world.map(LV2_ATOM__Float), &m_rate},
        {LV2_OPTIONS_INSTANCE, 0, world.map(LV2_UI__updateRate), sizeof(float),
         world.map(LV2_ATOM__Float), &m_updateRate},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    m_resize = {.handle = this, .ui_resize = &Lv2Ui::resize};
    m_dataAccess = {.data_access = effect.descriptor()->extension_data};
    m_parentFeature = {.URI = LV2_UI__parent,
                       .data = reinterpret_cast<void *>(m_window)}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr): LV2 passes the XID as a pointer
    m_resizeFeature = {.URI = LV2_UI__resize, .data = &m_resize};
    m_idleFeature = {.URI = LV2_UI__idleInterface, .data = nullptr};
    m_instanceFeature = {.URI = LV2_INSTANCE_ACCESS_URI, .data = effect.handle()};
    m_dataFeature = {.URI = LV2_DATA_ACCESS_URI, .data = &m_dataAccess};
    m_features = world.features(m_options.data());
    m_features.pop_back(); // its terminating null
    for (const LV2_Feature *f : {&m_parentFeature, &m_resizeFeature, &m_idleFeature,
                                 &m_instanceFeature, &m_dataFeature}) {
        m_features.push_back(f);
    }
    m_features.push_back(nullptr);

    // The UI itself. Its library stays loaded: some don't survive dlclose.
    void *library = dlopen(choice->binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        m_error = std::string("its window couldn't be loaded: ") + dlerror();
        close();
        return;
    }
    auto *entry = reinterpret_cast<LV2UI_DescriptorFunction>(dlsym(library, "lv2ui_descriptor")); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    for (std::uint32_t i = 0; entry != nullptr; ++i) {
        const LV2UI_Descriptor *d = entry(i);
        if (d == nullptr) {
            break;
        }
        if (choice->uri == d->URI) {
            m_descriptor = d;
            break;
        }
    }
    if (m_descriptor == nullptr) {
        m_error = "its window couldn't be found in its library";
        close();
        return;
    }
    m_sent.assign(m_effect.controlPorts().empty() ? 0 : m_effect.controlPorts().back() + 1,
                  std::numeric_limits<float>::quiet_NaN());
    LV2UI_Widget widget = nullptr;
    m_handle = m_descriptor->instantiate(
        m_descriptor, lilv_node_as_uri(lilv_plugin_get_uri(effect.plugin())),
        choice->bundle.c_str(), &Lv2Ui::write, this, &widget, m_features.data());
    if (m_handle == nullptr) {
        m_error = "its window couldn't be started";
        close();
        return;
    }
    m_child = reinterpret_cast<XWindowId>(widget); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (m_descriptor->extension_data != nullptr) {
        m_idle = static_cast<const LV2UI_Idle_Interface *>(
            m_descriptor->extension_data(LV2_UI__idleInterface));
        m_uiResize = static_cast<const LV2UI_Resize *>(m_descriptor->extension_data(LV2_UI__resize));
    }
    m_effect.setForwarding(true);

    // Sized to the UI: by its resize request, or else by its window.
    idle();
    XSync(m_display, False);
    if (m_child != 0) {
        Window root = 0;
        int x = 0;
        int y = 0;
        unsigned width = 0;
        unsigned height = 0;
        unsigned border = 0;
        unsigned depth = 0;
        if (XGetGeometry(m_display, m_child, &root, &x, &y, &width, &height, &border, &depth) != 0 &&
            width > 16 && height > 16) {
            resize(this, static_cast<int>(width), static_cast<int>(height));
        }
    }
    XMapRaised(m_display, m_window);
    XFlush(m_display);
}

Lv2Ui::~Lv2Ui()
{
    close();
}

void Lv2Ui::close()
{
    m_effect.setForwarding(false);
    if (m_handle != nullptr && m_descriptor != nullptr) {
        m_descriptor->cleanup(m_handle);
    }
    m_handle = nullptr;
    if (m_display != nullptr) {
        if (m_window != 0) {
            XDestroyWindow(m_display, m_window);
        }
        XCloseDisplay(m_display);
    }
    m_display = nullptr;
    m_window = 0;
}

bool Lv2Ui::idle()
{
    if (m_handle == nullptr || m_display == nullptr) {
        return false;
    }
    bool open = true;
    while (XPending(m_display) > 0) {
        XEvent event{};
        XNextEvent(m_display, &event);
        if (event.type == ClientMessage &&
            std::cmp_equal(event.xclient.data.l[0], m_deleteAtom)) { // NOLINT(cppcoreguidelines-pro-type-union-access)
            open = false;
        } else if (event.type == ConfigureNotify && event.xconfigure.window == m_window) { // NOLINT(cppcoreguidelines-pro-type-union-access)
            const int width = event.xconfigure.width;   // NOLINT(cppcoreguidelines-pro-type-union-access)
            const int height = event.xconfigure.height; // NOLINT(cppcoreguidelines-pro-type-union-access)
            if (width != m_width || height != m_height) {
                // The user resized the window: the UI follows.
                m_width = width;
                m_height = height;
                if (m_uiResize != nullptr) {
                    // A UI's own resize interface takes the UI's handle: the
                    // struct is shared by all its instances, so its handle
                    // field is meaningless (often null).
                    m_uiResize->ui_resize(m_handle, width, height);
                } else if (m_child != 0) {
                    XResizeWindow(m_display, m_child, static_cast<unsigned>(width),
                                  static_cast<unsigned>(height));
                }
            }
        }
    }
    if (!open) {
        return false;
    }

    // What changed since: control values (and meters), then events.
    for (const auto index : m_effect.controlPorts()) {
        const float value = m_effect.controlValue(index);
        if (index < m_sent.size() && !(m_sent[index] == value)) {
            m_sent[index] = value;
            m_descriptor->port_event(m_handle, index, sizeof(float), 0, &value);
        }
    }
    m_effect.takeEvents([this](std::uint32_t port, const LV2_Atom &atom) {
        m_descriptor->port_event(m_handle, port,
                                 static_cast<std::uint32_t>(sizeof(LV2_Atom) + atom.size),
                                 m_eventTransfer, &atom);
    });
    if (m_idle != nullptr && m_idle->idle(m_handle) != 0) {
        return false; // the UI closed itself
    }
    XFlush(m_display);
    return true;
}

void Lv2Ui::raise()
{
    if (m_display == nullptr) {
        return;
    }
    XMapRaised(m_display, m_window);
    // Ask the window manager to activate it (it may refuse focus stealing).
    XEvent event{};
    event.xclient.type = ClientMessage;                                         // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.window = m_window;                                            // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.message_type = XInternAtom(m_display, "_NET_ACTIVE_WINDOW", False); // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.format = 32;                                                  // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.data.l[0] = 2; // from a pager-like tool  // NOLINT(cppcoreguidelines-pro-type-union-access)
    XSendEvent(m_display, DefaultRootWindow(m_display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(m_display);
}

void Lv2Ui::write(LV2UI_Controller controller, std::uint32_t port, std::uint32_t size,
                  std::uint32_t protocol, const void *buffer)
{
    auto *self = static_cast<Lv2Ui *>(controller);
    if (protocol == 0 && size == sizeof(float)) {
        float value = 0.0F;
        std::memcpy(&value, buffer, sizeof(float));
        const auto symbol = self->m_effect.setControl(port, value);
        if (!symbol.empty()) {
            if (port < self->m_sent.size()) {
                self->m_sent[port] = self->m_effect.controlValue(port); // no echo
            }
            if (self->onParam) {
                self->onParam(symbol, self->m_effect.controlValue(port));
            }
        }
    } else if (protocol == self->m_eventTransfer) {
        self->m_effect.sendAtom(port, std::span(static_cast<const std::uint8_t *>(buffer), size));
    }
}

int Lv2Ui::resize(LV2UI_Feature_Handle handle, int width, int height)
{
    auto *self = static_cast<Lv2Ui *>(handle);
    if (self->m_display == nullptr || width <= 0 || height <= 0) {
        return 1;
    }
    self->m_width = width;
    self->m_height = height;
    XResizeWindow(self->m_display, self->m_window, static_cast<unsigned>(width),
                  static_cast<unsigned>(height));
    XFlush(self->m_display);
    return 0;
}

} // namespace pp

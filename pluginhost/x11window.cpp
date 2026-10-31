// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "x11window.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <print>
#include <utility>

namespace pp {

namespace {

// X errors a plugin's window causes (one already gone, say) aren't fatal.
int ignoreXError(Display * /*display*/, XErrorEvent *event)
{
    std::println(stderr, "patchprime-pluginhost: X error {} (request {})", event->error_code,
                 event->request_code);
    return 0;
}

} // namespace

void initX11Threads()
{
    XInitThreads();
}

X11Window::~X11Window()
{
    destroy();
}

bool X11Window::create(const std::string &title, int width, int height)
{
    m_display = XOpenDisplay(nullptr);
    if (m_display == nullptr) {
        return false;
    }
    XSetErrorHandler(ignoreXError);
    const int screen = DefaultScreen(m_display);
    m_width = width;
    m_height = height;
    m_window = XCreateSimpleWindow(m_display, RootWindow(m_display, screen), 0, 0,
                                   static_cast<unsigned>(width), static_cast<unsigned>(height), 0,
                                   BlackPixel(m_display, screen), BlackPixel(m_display, screen));
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
    return true;
}

void X11Window::show()
{
    if (m_display != nullptr) {
        XMapRaised(m_display, m_window);
        XFlush(m_display);
    }
}

void X11Window::raise()
{
    if (m_display == nullptr) {
        return;
    }
    XMapRaised(m_display, m_window);
    // Ask the window manager to activate it (it may refuse focus stealing).
    XEvent event{};
    event.xclient.type = ClientMessage;                                               // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.window = m_window;                                                  // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.message_type = XInternAtom(m_display, "_NET_ACTIVE_WINDOW", False); // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.format = 32;                                                        // NOLINT(cppcoreguidelines-pro-type-union-access)
    event.xclient.data.l[0] = 2; // NOLINT(cppcoreguidelines-pro-type-union-access): as a pager would
    XSendEvent(m_display, DefaultRootWindow(m_display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(m_display);
}

void X11Window::resize(int width, int height)
{
    if (m_display == nullptr || width <= 0 || height <= 0) {
        return;
    }
    m_width = width;
    m_height = height;
    XResizeWindow(m_display, m_window, static_cast<unsigned>(width), static_cast<unsigned>(height));
    XFlush(m_display);
}

void X11Window::resize(XWindowId window, int width, int height)
{
    if (m_display != nullptr && window != 0 && width > 0 && height > 0) {
        XResizeWindow(m_display, window, static_cast<unsigned>(width),
                      static_cast<unsigned>(height));
    }
}

std::optional<std::pair<int, int>> X11Window::sizeOf(XWindowId window) const
{
    if (m_display == nullptr || window == 0) {
        return std::nullopt;
    }
    XSync(m_display, False);
    Window root = 0;
    int x = 0;
    int y = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned border = 0;
    unsigned depth = 0;
    if (XGetGeometry(m_display, window, &root, &x, &y, &width, &height, &border, &depth) == 0) {
        return std::nullopt;
    }
    return std::pair{static_cast<int>(width), static_cast<int>(height)};
}

bool X11Window::pump(const std::function<void(int, int)> &resized)
{
    if (m_display == nullptr) {
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
                m_width = width;
                m_height = height;
                if (resized) {
                    resized(width, height);
                }
            }
        }
    }
    return open;
}

void X11Window::flush()
{
    if (m_display != nullptr) {
        XFlush(m_display);
    }
}

void X11Window::destroy()
{
    if (m_display == nullptr) {
        return;
    }
    if (m_window != 0) {
        XDestroyWindow(m_display, m_window);
    }
    XCloseDisplay(m_display);
    m_display = nullptr;
    m_window = 0;
}

} // namespace pp

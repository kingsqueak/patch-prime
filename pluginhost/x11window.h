// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

// Xlib's types, without its macros in every file that includes this.
using XDisplay = struct _XDisplay;
using XWindowId = unsigned long; // NOLINT(google-runtime-int): Xlib's Window

namespace pp {

// Makes Xlib safe for plugin UIs that use it from more than one thread; call
// first, before anything else touches X.
void initX11Threads();

// A plain top-level X11 window for a plugin's editor to be embedded in,
// with its own connection to the display. Main thread only.
class X11Window
{
public:
    X11Window() = default;
    ~X11Window();
    X11Window(const X11Window &) = delete;
    X11Window &operator=(const X11Window &) = delete;
    X11Window(X11Window &&) = delete;
    X11Window &operator=(X11Window &&) = delete;

    // Makes the window (not shown yet); false when there's no display.
    bool create(const std::string &title, int width, int height);
    [[nodiscard]] XWindowId id() const { return m_window; }
    [[nodiscard]] bool isOpen() const { return m_window != 0; }

    void show();
    void raise();
    // Sizes the window (from the program, not reported as the user's).
    void resize(int width, int height);
    // Be an XEmbed embedder (VST3 editors expect one): a window the editor
    // makes inside this one is told it's embedded, mapped, and given focus.
    // Call before the editor is attached.
    void setEmbedder(bool on);
    // Whether the user may resize it (the editor can follow). A window that
    // may not keeps the size it's given: the window manager is told so, and
    // offers no resizing.
    void setResizable(bool resizable);
    // Sizes another window (the editor in it, when the user resized this one).
    void resize(XWindowId window, int width, int height);
    // The size of another window (the editor's), or nullopt.
    [[nodiscard]] std::optional<std::pair<int, int>> sizeOf(XWindowId window) const;

    // Handles the window's events: resized(width, height) when the user
    // resized it. False when its close button was pressed.
    bool pump(const std::function<void(int, int)> &resized);
    void flush();
    void destroy();

private:
    XDisplay *m_display = nullptr;
    XWindowId m_window = 0;
    unsigned long m_deleteAtom = 0; // NOLINT(google-runtime-int): an X Atom
    int m_width = 0;
    int m_height = 0;
    bool m_resizable = true;
    bool m_embedder = false;

    void applySizeHints();
    void embed(XWindowId child);
};

} // namespace pp

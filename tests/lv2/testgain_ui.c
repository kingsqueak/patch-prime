// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// The test plugin's window: an empty X11 child of the host's window that, the
// first time it's idled, moves the gain to 0.5, as a user would. Enough to
// test that a window opens and that what's done in it reaches the engine.

#include <lv2/ui/ui.h>

#include <X11/Xlib.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GAIN_UI_URI "https://github.com/kingsqueak/patch-prime/tests/gain#ui"

enum { PORT_GAIN = 0 };

typedef struct {
    Display *display;
    Window window;
    LV2UI_Write_Function write;
    LV2UI_Controller controller;
    bool moved;
} TestUi;

static LV2UI_Handle instantiate(const LV2UI_Descriptor *descriptor, const char *plugin,
                                const char *bundle, LV2UI_Write_Function write,
                                LV2UI_Controller controller, LV2UI_Widget *widget,
                                const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)plugin;
    (void)bundle;
    Window parent = 0;
    for (int i = 0; features != NULL && features[i] != NULL; ++i) {
        if (strcmp(features[i]->URI, LV2_UI__parent) == 0) {
            parent = (Window)(uintptr_t)features[i]->data;
        }
    }
    TestUi *self = calloc(1, sizeof(TestUi));
    if (self == NULL || parent == 0 || (self->display = XOpenDisplay(NULL)) == NULL) {
        free(self);
        return NULL;
    }
    self->window = XCreateSimpleWindow(self->display, parent, 0, 0, 200, 100, 0, 0, 0);
    XMapWindow(self->display, self->window);
    XFlush(self->display);
    self->write = write;
    self->controller = controller;
    *widget = (LV2UI_Widget)(uintptr_t)self->window;
    return self;
}

static void cleanup(LV2UI_Handle handle)
{
    TestUi *self = handle;
    XDestroyWindow(self->display, self->window);
    XCloseDisplay(self->display);
    free(self);
}

static void port_event(LV2UI_Handle handle, uint32_t port, uint32_t size, uint32_t format,
                       const void *buffer)
{
    (void)handle;
    (void)port;
    (void)size;
    (void)format;
    (void)buffer;
}

static int idle(LV2UI_Handle handle)
{
    TestUi *self = handle;
    if (!self->moved) {
        const float gain = 0.5f;
        self->write(self->controller, PORT_GAIN, sizeof(gain), 0, &gain);
        self->moved = true;
    }
    return 0;
}

static const void *extension_data(const char *uri)
{
    static const LV2UI_Idle_Interface idleInterface = {idle};
    return strcmp(uri, LV2_UI__idleInterface) == 0 ? &idleInterface : NULL;
}

static const LV2UI_Descriptor descriptor = {
    TEST_GAIN_UI_URI, instantiate, cleanup, port_event, extension_data,
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}

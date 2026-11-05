// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// A tiny CLAP plugin for the tests, like tests/lv2/testgain.c: mono gain,
// a "crash" switch that kills its process, and a "halve" switch that halves
// its output for good (a trim kept only in its state). Halve turns itself
// off again through an output event, as a plugin's own editor would change
// a parameter, to test that the engine hears about such changes. A "restart"
// switch asks the host to restart it, and it halves its trim when that
// happens. Its window is an empty X11 child that moves the gain to 0.5 when
// shown, as a user would.

#include <clap/clap.h>

#include <X11/Xlib.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GAIN_ID "com.github.kingsqueak.patch-prime.tests.gain"

enum { PARAM_GAIN = 1, PARAM_CRASH = 2, PARAM_HALVE = 3, PARAM_RESTART = 4 };

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    double gain;
    double crash;
    double halve;
    float trim;
    bool reportHalve; // tell the host halve went back to 0
    double restart;
    bool restartAsked; // since the switch went on
    bool restarted;    // and it has been done
    bool reportGain;   // tell the host what its window did
    Display *display;
    Window window;
} TestGain;

static const char *const features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                       NULL};

static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = TEST_GAIN_ID,
    .name = "Patch Prime Test Gain (CLAP)",
    .vendor = "Patch Prime tests",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1",
    .description = "",
    .features = features,
};

// --- params

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return 4;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    (void)plugin;
    memset(info, 0, sizeof(*info));
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    switch (index) {
    case 0:
        info->id = PARAM_GAIN;
        strcpy(info->name, "Gain");
        info->max_value = 4.0;
        info->default_value = 1.0;
        return true;
    case 1:
        info->id = PARAM_CRASH;
        strcpy(info->name, "Crash");
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->max_value = 1.0;
        return true;
    case 3:
        info->id = PARAM_RESTART;
        strcpy(info->name, "Restart");
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->max_value = 1.0;
        return true;
    case 2:
        info->id = PARAM_HALVE;
        strcpy(info->name, "Halve");
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->max_value = 1.0;
        return true;
    default:
        return false;
    }
}

static double *param(TestGain *self, clap_id id)
{
    switch (id) {
    case PARAM_GAIN:
        return &self->gain;
    case PARAM_CRASH:
        return &self->crash;
    case PARAM_HALVE:
        return &self->halve;
    case PARAM_RESTART:
        return &self->restart;
    default:
        return NULL;
    }
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    double *p = param(plugin->plugin_data, id);
    if (p == NULL) {
        return false;
    }
    *value = *p;
    return true;
}

static void apply_events(TestGain *self, const clap_input_events_t *in)
{
    for (uint32_t i = 0; in != NULL && i < in->size(in); ++i) {
        const clap_event_header_t *event = in->get(in, i);
        if (event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t *value = (const clap_event_param_value_t *)event;
            double *p = param(self, value->param_id);
            if (p != NULL) {
                *p = value->value;
            }
        }
    }
    if (self->halve > 0.5) {
        self->trim = 0.5f;
        self->halve = 0.0;
        self->reportHalve = true;
    }
    if (self->restart > 0.5 && !self->restartAsked) {
        self->restartAsked = true;
        self->host->request_restart(self->host); // allowed on the audio thread
    } else if (self->restart <= 0.5) {
        self->restartAsked = false;
        self->restarted = false;
    }
}

static bool report_param(const clap_output_events_t *out, clap_id id, double value)
{
    const clap_event_param_value_t event = {
        .header = {.size = sizeof(event),
                   .time = 0,
                   .space_id = CLAP_CORE_EVENT_SPACE_ID,
                   .type = CLAP_EVENT_PARAM_VALUE,
                   .flags = 0},
        .param_id = id,
        .cookie = NULL,
        .note_id = -1,
        .port_index = -1,
        .channel = -1,
        .key = -1,
        .value = value,
    };
    return out->try_push(out, &event.header);
}

static void report(TestGain *self, const clap_output_events_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self->reportHalve && report_param(out, PARAM_HALVE, 0.0)) {
        self->reportHalve = false;
    }
    if (self->reportGain && report_param(out, PARAM_GAIN, 0.5)) {
        self->reportGain = false;
    }
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                         const clap_output_events_t *out)
{
    apply_events(plugin->plugin_data, in);
    report(plugin->plugin_data, out);
}

static const clap_plugin_params_t params = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = NULL,
    .text_to_value = NULL,
    .flush = params_flush,
};

// --- audio ports: one mono main input and output

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin;
    (void)is_input;
    return 1;
}

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index != 0) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->id = is_input ? 0 : 1;
    strcpy(info->name, is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 1;
    info->port_type = CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t ports = {.count = ports_count, .get = ports_get};

// --- state: the trim

// Streams may move fewer bytes than asked: loop until all are through (0 is
// the end, -1 an error).
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    const TestGain *self = plugin->plugin_data;
    const char *at = (const char *)&self->trim;
    uint64_t left = sizeof(self->trim);
    while (left > 0) {
        const int64_t n = stream->write(stream, at, left);
        if (n <= 0) {
            return false;
        }
        at += n;
        left -= (uint64_t)n;
    }
    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    TestGain *self = plugin->plugin_data;
    float trim = 1.0f;
    char *at = (char *)&trim;
    uint64_t left = sizeof(trim);
    while (left > 0) {
        const int64_t n = stream->read(stream, at, left);
        if (n <= 0) {
            return false;
        }
        at += n;
        left -= (uint64_t)n;
    }
    self->trim = trim;
    return true;
}

static const clap_plugin_state_t state = {.save = state_save, .load = state_load};

// --- its window

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool floating)
{
    (void)plugin;
    return strcmp(api, CLAP_WINDOW_API_X11) == 0 && !floating;
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *floating)
{
    (void)plugin;
    *api = CLAP_WINDOW_API_X11;
    *floating = false;
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool floating)
{
    TestGain *self = plugin->plugin_data;
    self->display = gui_is_api_supported(plugin, api, floating) ? XOpenDisplay(NULL) : NULL;
    return self->display != NULL;
}

static void gui_destroy(const clap_plugin_t *plugin)
{
    TestGain *self = plugin->plugin_data;
    if (self->window != 0) {
        XDestroyWindow(self->display, self->window);
        self->window = 0;
    }
    XCloseDisplay(self->display);
    self->display = NULL;
}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale)
{
    (void)plugin;
    (void)scale;
    return false;
}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    (void)plugin;
    *width = 200;
    *height = 100;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin)
{
    (void)plugin;
    return false;
}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
    (void)plugin;
    (void)hints;
    return false;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    return gui_get_size(plugin, width, height);
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    (void)plugin;
    (void)width;
    (void)height;
    return false;
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    TestGain *self = plugin->plugin_data;
    self->window = XCreateSimpleWindow(self->display, window->x11, 0, 0, 200, 100, 0, 0, 0);
    XMapWindow(self->display, self->window);
    XFlush(self->display);
    return true;
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
    (void)plugin;
    (void)window;
    return false;
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
    (void)plugin;
    (void)title;
}

static bool gui_show(const clap_plugin_t *plugin)
{
    TestGain *self = plugin->plugin_data;
    self->reportGain = true; // on the next block
    return true;
}

static bool gui_hide(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static const clap_plugin_gui_t gui = {
    .is_api_supported = gui_is_api_supported,
    .get_preferred_api = gui_get_preferred_api,
    .create = gui_create,
    .destroy = gui_destroy,
    .set_scale = gui_set_scale,
    .get_size = gui_get_size,
    .can_resize = gui_can_resize,
    .get_resize_hints = gui_get_resize_hints,
    .adjust_size = gui_adjust_size,
    .set_size = gui_set_size,
    .set_parent = gui_set_parent,
    .set_transient = gui_set_transient,
    .suggest_title = gui_suggest_title,
    .show = gui_show,
    .hide = gui_hide,
};

// --- the plugin

static bool plugin_init(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    free(plugin->plugin_data);
}

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min, uint32_t max)
{
    (void)rate;
    (void)min;
    (void)max;
    TestGain *self = plugin->plugin_data;
    if (self->restartAsked && !self->restarted) {
        self->restarted = true;
        self->trim *= 0.5f; // the sign that it really happened
    }
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    TestGain *self = plugin->plugin_data;
    apply_events(self, process->in_events);
    if (self->crash > 0.5) {
        raise(SIGKILL);
    }
    const float gain = (float)self->gain * self->trim;
    const float *in = process->audio_inputs[0].data32[0];
    float *out = process->audio_outputs[0].data32[0];
    for (uint32_t i = 0; i < process->frames_count; ++i) {
        out[i] = in[i] * gain;
    }
    report(self, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &params;
    }
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &ports;
    }
    if (strcmp(id, CLAP_EXT_STATE) == 0) {
        return &state;
    }
    if (strcmp(id, CLAP_EXT_GUI) == 0) {
        return &gui;
    }
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    (void)plugin;
}

// --- the factory and entry

static uint32_t factory_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return 1;
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory,
                                                          uint32_t index)
{
    (void)factory;
    return index == 0 ? &descriptor : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory,
                                           const clap_host_t *host, const char *id)
{
    (void)factory;
    if (strcmp(id, TEST_GAIN_ID) != 0) {
        return NULL;
    }
    TestGain *self = calloc(1, sizeof(TestGain));
    if (self == NULL) {
        return NULL;
    }
    self->host = host;
    self->gain = 1.0;
    self->trim = 1.0f;
    self->plugin = (clap_plugin_t){
        .desc = &descriptor,
        .plugin_data = self,
        .init = plugin_init,
        .destroy = plugin_destroy,
        .activate = plugin_activate,
        .deactivate = plugin_deactivate,
        .start_processing = plugin_start_processing,
        .stop_processing = plugin_stop_processing,
        .reset = plugin_reset,
        .process = plugin_process,
        .get_extension = plugin_get_extension,
        .on_main_thread = plugin_on_main_thread,
    };
    return &self->plugin;
}

static const clap_plugin_factory_t factory = {
    .get_plugin_count = factory_count,
    .get_plugin_descriptor = factory_descriptor,
    .create_plugin = factory_create,
};

static bool entry_init(const char *path)
{
    (void)path;
    printf("testgain (CLAP): loading\n"); // chatter, as real plugins do
    fflush(stdout);
    return true;
}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id)
{
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};

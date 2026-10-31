// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// A tiny LV2 plugin for the tests: mono gain (a factor), a "crash" switch
// that kills its process (to test that a crashing plugin doesn't take its
// strip down), and a "halve" switch that halves its output from then on,
// for good: a trim kept only in its own state (state:interface), to test
// that state is saved and restored.

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GAIN_URI "https://github.com/kingsqueak/patch-prime/tests/gain"

enum { PORT_GAIN = 0, PORT_CRASH = 1, PORT_IN = 2, PORT_OUT = 3, PORT_HALVE = 4 };

typedef struct {
    const float *gain;
    const float *crash;
    const float *in;
    float *out;
    const float *halve;
    float trim;
    LV2_URID trimKey;
    LV2_URID floatType;
} TestGain;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate, const char *path,
                              const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)rate;
    (void)path;
    TestGain *self = calloc(1, sizeof(TestGain));
    if (self == NULL) {
        return NULL;
    }
    self->trim = 1.0f;
    for (int i = 0; features != NULL && features[i] != NULL; ++i) {
        if (strcmp(features[i]->URI, LV2_URID__map) == 0) {
            const LV2_URID_Map *map = (const LV2_URID_Map *)features[i]->data;
            self->trimKey = map->map(map->handle, TEST_GAIN_URI "#trim");
            self->floatType = map->map(map->handle, LV2_ATOM__Float);
        }
    }
    return self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    TestGain *self = (TestGain *)instance;
    switch (port) {
    case PORT_GAIN:
        self->gain = (const float *)data;
        break;
    case PORT_CRASH:
        self->crash = (const float *)data;
        break;
    case PORT_IN:
        self->in = (const float *)data;
        break;
    case PORT_OUT:
        self->out = (float *)data;
        break;
    case PORT_HALVE:
        self->halve = (const float *)data;
        break;
    default:
        break;
    }
}

static void run(LV2_Handle instance, uint32_t frames)
{
    TestGain *self = (TestGain *)instance;
    if (*self->crash > 0.5f) {
        raise(SIGKILL);
    }
    if (*self->halve > 0.5f) {
        self->trim = 0.5f;
    }
    const float gain = *self->gain * self->trim;
    for (uint32_t i = 0; i < frames; ++i) {
        self->out[i] = self->in[i] * gain;
    }
}

static void cleanup(LV2_Handle instance)
{
    free(instance);
}

static LV2_State_Status save(LV2_Handle instance, LV2_State_Store_Function store,
                             LV2_State_Handle handle, uint32_t flags,
                             const LV2_Feature *const *features)
{
    (void)flags;
    (void)features;
    TestGain *self = (TestGain *)instance;
    if (self->trimKey == 0) {
        return LV2_STATE_ERR_NO_FEATURE;
    }
    return store(handle, self->trimKey, &self->trim, sizeof(float), self->floatType,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                                LV2_State_Handle handle, uint32_t flags,
                                const LV2_Feature *const *features)
{
    (void)flags;
    (void)features;
    TestGain *self = (TestGain *)instance;
    size_t size = 0;
    uint32_t type = 0;
    uint32_t valueFlags = 0;
    const void *value = retrieve(handle, self->trimKey, &size, &type, &valueFlags);
    if (value != NULL && size == sizeof(float) && type == self->floatType) {
        memcpy(&self->trim, value, sizeof(float));
    }
    return LV2_STATE_SUCCESS;
}

static const void *extension_data(const char *uri)
{
    static const LV2_State_Interface state = {save, restore};
    return strcmp(uri, LV2_STATE__interface) == 0 ? &state : NULL;
}

static const LV2_Descriptor descriptor = {
    TEST_GAIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}

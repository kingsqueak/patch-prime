// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// A tiny LV2 plugin for the tests: mono gain (a factor), and a "crash"
// switch that kills its process, to test that a crashing plugin doesn't
// take its strip down.

#include <lv2/core/lv2.h>

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

#define TEST_GAIN_URI "https://github.com/kingsqueak/patch-prime/tests/gain"

enum { PORT_GAIN = 0, PORT_CRASH = 1, PORT_IN = 2, PORT_OUT = 3 };

typedef struct {
    const float *gain;
    const float *crash;
    const float *in;
    float *out;
} TestGain;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate, const char *path,
                              const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)rate;
    (void)path;
    (void)features;
    return calloc(1, sizeof(TestGain));
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
    const float gain = *self->gain;
    for (uint32_t i = 0; i < frames; ++i) {
        self->out[i] = self->in[i] * gain;
    }
}

static void cleanup(LV2_Handle instance)
{
    free(instance);
}

static const LV2_Descriptor descriptor = {
    TEST_GAIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}

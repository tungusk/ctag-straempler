#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

// A "machine" is an independent sampling/processing mode. Exactly one machine
// is active at a time. The core owns the hardware transport (I2S, CV
// acquisition, SD, display shell, WiFi) and hands the active machine one
// audio block at a time; the machine owns everything between input and
// output: its engine, its parameters/preset schema, and its menu pages.
//
// Machines are compiled into the firmware and listed in machine_registry[]
// (machine_registry.c). A build may ship any subset — core code must never
// reference a concrete machine symbol directly, so that a sampler-less
// build still links.

#define MACHINE_BUF_SZ 64   // samples per block per channel @ 44100 Hz

typedef struct {
    // rising-edge flags for the hardware trigger inputs, valid this block
    uint8_t trig_rising;    // bit0 = TRIG0, bit1 = TRIG1
    uint8_t trig_level;     // current levels, same bit layout
    // corrected CV snapshot (cv_corrected already applied), 0..4095
    uint16_t cv[8];
} machine_io_t;

typedef struct machine_s {
    const char *name;       // shown in the machine selector, <=15 chars

    // Lifecycle. start() allocates buffers/tasks and registers menu pages;
    // stop() must free everything it allocated — the next machine needs the
    // RAM. Both run outside the audio task; audio is muted around them.
    esp_err_t (*start)(void);
    void      (*stop)(void);

    // Audio: called once per block from the audio task (core 1, ~1.45 ms).
    // in  = interleaved stereo line-in as read from the codec
    // out = interleaved stereo to the codec, pre-zeroed by the core
    // No SD access, no blocking calls, no heap allocation in here.
    void (*process)(int32_t out[MACHINE_BUF_SZ * 2],
                    const int32_t in[MACHINE_BUF_SZ * 2],
                    const machine_io_t *io);

    // Preset persistence: the core owns the bank/CONFIG containers and calls
    // these with a machine-private JSON node ({"machine":"<name>", ...}).
    void (*preset_save)(cJSON *node);
    void (*preset_load)(const cJSON *node);
} machine_t;

// compile-time registry, terminated by NULL (machine_registry.c)
extern const machine_t *const machine_registry[];

// core-side accessors (machine_core.c)
const machine_t *machine_active(void);
esp_err_t machine_activate(const machine_t *m);   // stop old, start new
const machine_t *machine_by_name(const char *name);

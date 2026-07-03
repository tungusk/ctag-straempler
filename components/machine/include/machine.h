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

// One block = 64 interleaved int32 slots = 32 stereo frames @ 44100 Hz.
#define MACHINE_BLOCK 64

typedef struct {
    // raw GPIO levels of the trigger inputs (active low on this hardware);
    // bit0 = TRIG0, bit1 = TRIG1. Edge derivation is machine policy for now
    // (the sampler has latch modes); may move core-side later.
    uint8_t trig_level;
    uint8_t trig_rising;    // reserved, 0 until edge detection moves core-side
    // corrected CV snapshot (cv_corrected applied), 0..4095 — use this
    uint16_t cv[8];
    // TEMPORARY (until M0b): raw ADC values for the legacy sampler code,
    // which applies cv_corrected() itself at every modulation call site.
    uint16_t cv_raw[8];
} machine_io_t;

// A machine's presence in the menu system. All hooks optional (NULL).
// For now machine pages use IDs from the app-wide menu enum; a dedicated
// machine ID space arrives with the physical menu split (M0c-2).
typedef struct {
    const char *const *main_items;  // labels for the main-menu entries
    const int *main_targets;        // menusys item id each entry opens
    int n_main;
    void (*register_pages)(void *menusys);       // create the machine's pages
    int  (*main_event)(int event, void *ev_data); // main-screen live area
} machine_ui_t;

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
    void (*process)(int32_t out[MACHINE_BLOCK],
                    const int32_t in[MACHINE_BLOCK],
                    const machine_io_t *io);

    // Preset persistence: the core owns the bank/CONFIG containers and calls
    // these with a machine-private JSON node ({"machine":"<name>", ...}).
    void (*preset_save)(cJSON *node);
    void (*preset_load)(const cJSON *node);

    const machine_ui_t *ui;   // menu integration, optional
} machine_t;

// compile-time registry, terminated by NULL (machine_registry.c)
extern const machine_t *const machine_registry[];

// core-side accessors (machine_core.c)
const machine_t *machine_active(void);
esp_err_t machine_activate(const machine_t *m);   // stop old, start new
const machine_t *machine_by_name(const char *name);

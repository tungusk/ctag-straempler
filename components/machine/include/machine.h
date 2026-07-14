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
    // bit0 = TRIG0, bit1 = TRIG1. Edge derivation from LEVEL is machine
    // policy (the sampler has latch modes) — but see trig_rising below.
    uint8_t trig_level;
    // one bit per VALIDATED gate-assert edge since the previous block,
    // measured by GPIO ISR (audio.c): the low must hold >= 200 us. A 1 ms
    // gate that falls entirely between block samples still registers here,
    // and a sub-100 us floating-input glitch never does. Teleremote soft
    // assert edges are merged in. Prefer this over deriving edges from
    // trig_level when short external triggers must be honored.
    uint8_t trig_rising;
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
    // page to land on after boot/machine switch; 0 = the main menu screen
    // (machines with a dedicated live view set this to it — the samplers'
    // home IS the main screen, so they leave it 0)
    int boot_target;
    // optional web endpoints, served only while this machine is active:
    // points to a const httpd_uri_t[n_web_uris] (kept void* so machine.h
    // doesn't drag httpd types into every machine build). Handlers must only
    // touch state that survives stop() — an in-flight request can still be
    // executing while the machine is being switched away.
    const void *web_uris;
    int n_web_uris;
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

    // Preset persistence: the core owns the autosave container
    // ({"machine":"<name>","state":{...}} in /sdcard/AUTOSAVE.JSN).
    // preset_save returns a freshly allocated state node (core frees it);
    // preset_load applies a node, or loads machine defaults when NULL
    // (missing file or a different machine's autosave).
    cJSON *(*preset_save)(void);
    void   (*preset_load)(const cJSON *node);

    const machine_ui_t *ui;   // menu integration, optional
} machine_t;

// compile-time registry, terminated by NULL (machine_registry.c)
extern const machine_t *const machine_registry[];

// core-side accessors (machine_core.c)
const machine_t *machine_active(void);
esp_err_t machine_activate(const machine_t *m);   // stop old, start new
const machine_t *machine_by_name(const char *name);

// rest-api hookup: the web server installs a callback fired on every
// activate/deactivate (m = new machine, NULL while switching/stopped).
// Installing fires immediately with the currently active machine, so the
// boot order of WiFi vs. machine start doesn't matter. The server uses it
// to (un)register the machine's web_uris.
void machine_set_web_cb(void (*cb)(const machine_t *m));

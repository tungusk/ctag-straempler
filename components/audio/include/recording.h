#pragma once
#include <stdbool.h>
#include <stdint.h>

// What a trigger pin does when it fires.
typedef enum {
    TRIG_FUNC_VOICE = 0,     // trigger the assigned voice (default)
    TRIG_FUNC_RECORD,        // gate controls recording only, voice suspended
    TRIG_FUNC_TRANSPORT,     // gate controls recording AND triggers the voice
} trig_func_t;

void recording_init(void);
void recording_start(void);
void recording_stop(void);
bool recording_is_active(void);
void recording_push(const int32_t *samples);

// Per-trigger function routing (vid 0 = TRIG0, vid 1 = TRIG1).
void recording_set_trig_func(int vid, trig_func_t func);
trig_func_t recording_get_trig_func(int vid);

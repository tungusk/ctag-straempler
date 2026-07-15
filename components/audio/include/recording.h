#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TRIG_FUNC_VOICE = 0,
    TRIG_FUNC_RECORD,
} trig_func_t;

void recording_init(void);
void recording_start(int vid);  // vid: which voice slot to auto-load into after stop
void recording_stop(void);
bool recording_is_active(void);
void recording_push(const int32_t *samples);
uint32_t recording_get_drops(void);  // capture chunks lost to a full queue

// Clock-synced capture (sampler3): prepare opens the file and parks the
// writer so the actual start costs nothing; trigger/finish are bare atomic
// stores — safe to call from the audio task ON a clock pulse (no logs, no
// SD, no allocation). recording_start(vid) == prepare + trigger.
void recording_prepare(int vid);
bool recording_is_prepared(void);
void recording_trigger(void);          // audio-task safe
void recording_finish(void);           // audio-task safe
void recording_cancel_prepared(void);  // disarm before trigger: file deleted

// Returns true once (clears flag) when a recording has been saved and is ready to load.
// Fills vid_out and fname_out (must be at least 48 bytes).
bool recording_poll_load(int *vid_out, char *fname_out);

void recording_set_trig_func(int vid, trig_func_t func);
trig_func_t recording_get_trig_func(int vid);
int recording_get_target_vid(void);
void recording_set_prefix(const char *p);   // take-name prefix ("REC"/"BNC")
void recording_set_enabled(bool en);
bool recording_get_enabled(void);
void recording_set_arm_monitor(bool en);
bool recording_get_arm_monitor(void);

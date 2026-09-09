#pragma once
#include <stdbool.h>
#include "clock.h"

// Setup-row helpers for the CORE clock (clock.h): every clocked machine's
// "Clock Src" / "PPQ" / "BPM" rows write THROUGH to the module-wide setting.
// Each call applies live at once; persistence to CONFIG.JSN is debounced
// through the menu's autosave path (a detent-by-detent BPM turn must not write
// the card per click — configSetIntSetting rewrites the whole file, ~64 ms
// under sd_lock, during playback). clock_ui_flush() is called from
// autosave_now(); the setters flag machine_state_dirty() to arm it.
int   clock_ui_cycle_src(int dir);      // CV1..8 > AUDIO > INT > OFF (no TR)
void  clock_ui_set_src(int src);        // CLK_SRC_*
float clock_ui_cycle_ppq(int dir);      // 1 > 2 > 4 > 8, returns the new value
const char *clock_ui_ppq_name(void);    // "4 per beat" (Setup row text)
float clock_ui_adj_bpm(float d);        // INT / fallback tempo, 20..300
bool  clock_ui_toggle_auto(void);       // returns the new state
void  clock_ui_flush(void);             // persist pending changes (autosave path)

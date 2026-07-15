#pragma once
#include <stdbool.h>

// Shared TWO-LEVEL sample browser — the folder screen (all/pool/REC/LOOPS
// with live counts) that opens into the centered big-name list every load
// page already used. Factored from six line-identical copies (deck, dualdeck,
// drums, granular, slicer, sampler3); the machine keeps its own menusys page
// and load callback, this owns the drawing and the browse state. One browser
// runs at a time (the sample_ram shared-buffer invariant), so the instance is
// a single static inside sample_browser.c.
//
// Folders are organization, not restriction (Arlo): every file stays
// reachable through "all", and per-folder lists turn the 224/512 caps into
// per-folder budgets a grown library can actually live inside.

// enter at the folder screen. recent: true = dated newest-first provider
// (sample_list_recent_dir), false = alphabetical (sample_list_shared_dir).
// title: shown over the list ("Load Sample", "Pad 3 A", ...). current: the
// machine's loaded sample name — the list selection seeds onto it (may be
// NULL/empty).
void sample_browser_enter(bool recent, const char *title, const char *current);

// Like sample_browser_enter, but FORCES the open to land in folder `dir` (a
// SAMPLE_DIR_* value) instead of the remembered position — for a machine with a
// natural home folder (Drums -> usr/DRUMS). Selection lands on that folder's
// first file. `dir` out of range falls back to the flat ALL view.
void sample_browser_enter_dir(bool recent, const char *title, const char *current, int dir);

// feed menusys events. Returns:
//   0 = handled, stay in the browser page
//   1 = user picked a sample -> sample_browser_selected(), leave the page
//   2 = user cancelled out (hold on the folder screen), leave the page
int sample_browser_event(int event);

const char *sample_browser_selected(void);

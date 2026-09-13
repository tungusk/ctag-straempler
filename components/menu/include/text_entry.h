#pragma once

// Shared encoder TEXT ENTRY — the character-picker every typed string on the
// panel goes through. Factored (2026-09-12) out of settings_input_def_handler,
// which had been the only live copy: the WiFi SSID / Password / Api Key page.
// That page is now this widget's first host, so "the settings page still types
// a password correctly" is the regression test for the extraction.
//
// Grammar, unchanged from the settings page:
//   TURN         scroll the character under the cursor
//   SHORT PRESS  commit it and advance
//   LONG PRESS   commit it UPPERCASED and advance
// and three sentinels live in the character list itself, reached by turning
// past 'z':  '<' backspace, '^' cancel, '=' accept.
//
// One entry at a time — the state is a single file-static, the same invariant
// sample_browser holds. Hosts keep their own menusys page and act on the
// return code, exactly like sample_browser_event().
//
// NOTE ON NAMING POLICY: preset_store's "no on-device text entry" rule is about
// pool IDS (FatFS 8.3, auto-numbered <PFX>NNNN) and still stands. This widget
// is for strings that are not ids — a password, an API key, a search query.

#define TEXT_ENTRY_MAX 47          // longest string the widget will return

// Open the entry page. `title` is the prompt ("Enter Password:", "Search:").
// `initial` seeds the buffer and parks the cursor at its end (NULL/"" = empty).
// `maxlen` caps the typed length; <=0 or oversized clamps to TEXT_ENTRY_MAX.
// DRAWS — call it from EV_ENTERED_MENU, never from a REST/remote path.
void text_entry_enter(const char *title, const char *initial, int maxlen);

// Feed a menusys event. Returns:
//   0 = handled, stay on the page
//   1 = accepted ('=')  -> read text_entry_result()
//   2 = cancelled ('^') -> leave the page, discard
int text_entry_event(int event);

// The accepted string. Valid until the next text_entry_enter().
const char *text_entry_result(void);

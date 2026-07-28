#pragma once
// ESP-NOW coexistence SPIKE — see espnow_probe.c. Measurement scaffolding for the
// ad-hoc unit-to-unit sync plan, not a feature. Idle until set_hz is called.
#ifdef __cplusplus
extern "C" {
#endif

// Broadcast a small packet at `hz` per second (0 = stop). Initialises ESP-NOW on
// the first non-zero call, so this must not run before initWifi. Returns 0 on ok.
int  espnow_probe_set_hz(int hz);
void espnow_probe_stats(int *hz, unsigned *sent, unsigned *failed);
void espnow_probe_reset_stats(void);

#ifdef __cplusplus
}
#endif

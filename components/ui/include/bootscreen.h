#pragma once
// The verbose boot screen, shown when the card has no bootlogo.bmp (or there
// is no card): the name, the version tag, the caller's info lines, a quip.
// Draws into the current TFT state; the caller holds the delay.
void bootscreen_draw(const char *version, const char *const *lines, int n);
void bootscreen_line(const char *text);     // append a log line (e.g. the ip, once WiFi is up)

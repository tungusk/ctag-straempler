#pragma once
// The text-only boot screen, shown when the card has no bootlogo.bmp (or there
// is no card). Draws into the current TFT state; the caller holds the delay.
void bootscreen_draw(const char *version);

#pragma once
#include "esp_wifi.h"

void initWifi(void);
void wifiWaitForConnected();
int isWiFiConnected();
void restartWifi(wifi_config_t *cfg);
void wifiApplyTxPower(int quarter_dbm);  // 8..84 quarter-dBm (2..21 dBm)
void wifiGetIPString(char *out, int len); // dotted quad, or "no IP"
void wifiApplyHostname(const char *name);  // live mdns rename (persist separately)

// ESP-NOW coexistence spike (see espnow_probe.c). Routed through wifi.c so the
// probe object is guaranteed to link — see the note beside the definitions.
int  wifiEspnowSetHz(int hz);
void wifiEspnowStats(int *hz, unsigned *sent, unsigned *failed);
void wifiEspnowResetStats(void);

#pragma once
#include "esp_wifi.h"

void initWifi(void);
void wifiWaitForConnected();
int isWiFiConnected();
void restartWifi(wifi_config_t *cfg);
void wifiApplyTxPower(int quarter_dbm);  // 8..84 quarter-dBm (2..21 dBm)
void wifiGetIPString(char *out, int len); // dotted quad, or "no IP"
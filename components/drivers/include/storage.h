#pragma once

#include <stddef.h>
void mountSDStorage();
int sdStorageInfo(char *out, size_t len);   // 1 + "31.3 GB SDHC" when a card is mounted
void unmountSDStorage();

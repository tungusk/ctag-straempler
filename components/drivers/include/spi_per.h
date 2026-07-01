#pragma once
#include <stdbool.h>

void initSpiPer(xQueueHandle queueui);
void codec_set_input(bool use_mic);

#pragma once

void initUI();

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
xQueueHandle uiGetEventQueue(void);

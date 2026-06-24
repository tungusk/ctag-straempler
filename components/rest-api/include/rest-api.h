#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void startRestAPI(xQueueHandle queueui);
void stopRestAPI(void);
void setRestAPIUserReceiveOn(void);
void setRestAPIUserReceiveOff(void);

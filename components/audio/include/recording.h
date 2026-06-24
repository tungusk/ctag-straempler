#pragma once
#include <stdbool.h>
#include <stdint.h>

void recording_init(void);
void recording_start(void);
void recording_stop(void);
bool recording_is_active(void);
void recording_push(const int32_t *samples);

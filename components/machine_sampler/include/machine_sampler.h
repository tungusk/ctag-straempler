#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "machine.h"
#include "audio_types.h"

// Referenced only by main/machine_registry.c.
extern const machine_t machine_sampler;

// Boot wiring called from initUI — goes away in M0c when the menus (and the
// queues that feed them) move into this machine.
void sampler_bind_queues(xQueueHandle v0, xQueueHandle v1, xQueueHandle eff,
                         xQueueHandle pbs0, xQueueHandle pbs1,
                         xQueueHandle mode0, xQueueHandle mode1,
                         xQueueHandle matrix_ev, xQueueHandle ui_ev);
void sampler_boot_init(void);
void sampler_menu_bind_queues(xQueueHandle v0, xQueueHandle v1, xQueueHandle eff,
                              xQueueHandle pbs0, xQueueHandle pbs1,
                              xQueueHandle mode0, xQueueHandle mode1,
                              xQueueHandle matrix_ev, xQueueHandle ui_ev);

// menu/UI-facing sampler API (the menus themselves move here in M0c)
void assignAudioFiles();
void enableTrigModeLatch(uint8_t vid);
void disableTrigModeLatch(uint8_t vid);
void getAudioBasename(int vid, char *out, int len);
bool isVoicePlaying(int vid);
void audio_get_playpos(int vid, uint32_t *sample_start, uint32_t *loop_start, uint32_t *loop_end, uint32_t *fsize);

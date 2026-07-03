#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cJSON.h"

void initMenu(xQueueHandle ev_queue);
void menuProcessEvent(int ev, void * ev_data);
void menuBindMachineUI(void);

// menusys pass-around globals (see menusys.h contract)
extern void *_state_data;
extern void *_state_voice;
extern void *_state_json;
extern void *_fb_state;
void initParams();
void loadParams(cJSON *filename);

/*
void fwdMenu();
void bwdMenu();
void selectMenuLong();
void selectMenuShort();
void updateMenuTime();
void updateTagList(void*);
*/
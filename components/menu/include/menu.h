#pragma once
#include "cJSON.h"

void initMenu();
void menuProcessEvent(int ev, void * ev_data);
void menuBindMachineUI(void);

// menusys pass-around globals (see menusys.h contract)
extern void *_state_data;
extern void *_state_voice;
extern void *_state_json;
extern void *_fb_state;
void initParams();
cJSON* buildPreset();
void loadParams(cJSON *filename);

/*
void fwdMenu();
void bwdMenu();
void selectMenuLong();
void selectMenuShort();
void updateMenuTime();
void updateTagList(void*);
*/
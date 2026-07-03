#pragma once
// sampler machine display routines (definitions in sampler_tft.c)
#include <stdbool.h>
#include "cJSON.h"
#include "list.h"
#include "audio_types.h"
#include "menu_types.h"
#include "menu_envelope.h"
#include "menu_playmode.h"

void menuTFTPrintVoiceMenu();
void menuTFTPrintADSRMenu();
void menuTFTPrintPlaymodeMenu();
void menuTFTPrintMatrixMenu();
void menuTFTPrintSlotMenu(const cJSON *slotData, int activeSlot);
void menuTFTPrintBrowseTextMenu();
void menuTFTPrintLoadingTagMenu();
void menuTFTPrintUserFileMenu();
void menuTFTPrintSelectIDMenu();
void menuTFTPrintPresetLayout();
void menuTFTPrintPresetList(cJSON* bank, print_ids_t action);
void menuTFTPrintBankList(list_t* list, print_ids_t action);
void menuTFTSelectVoiceMenu(int, int);
void menuTFTSelectMatrixItem(int active, int select, int column);
void menuTFTSelectPreset(int* activeSlot, cJSON* rootArray, int selected);
void menuTFTSelectPresetBank(int* activeSlot, list_t* list, int selected);
void menuTFTPrintVoiceValues(param_data_t* data, int);
void menuTFTPrintADSRValues(adsr_data_t* data, int);
void menuTFTPrintFilterValues(filter_data_t* data, int);
void menuTFTPrintPlaymodeValues(play_state_data_t* data, int);
void menuTFTPrintMatrixAmount(matrix_ui_row_t* matrix, int);
void menuTFTPrintMatrixDestination(matrix_ui_row_t* matrix, int);
void menuTFTPrintDelayValues(delay_data_t* data, int);
void menuTFTPrintExtInValues(ext_in_data_t* data, int);
void menuTFTPrintPlaymodeIndicators(play_state_data_t* data, int);
void menuTFTInitPlaymodeIndicators();
void menuTFTPrintADSRCurve(adsr_data_t* data);
void menuTFTPrintMatrixRowIndicator(int select, int row);
void menuTFTInitAdsrCurve();
void menuTFTPrintCurrentSettings(param_data_t*);
void menuTFTPrintCurrentPresetSettings(char* title, char* data);
void menuTFTFeedbackMenuItemHSpaced(int *cnt, int* pos, const char** items, const int* n_items, int state);
void menuTFTUpdateProgress(char *text, int progress);
void menuTFTPrintBrowseTagList(list_t*);
int printTags(list_item_t* it);
void menuTFTPrintFileBrowser(list_t* files, int current);
void menuTFTPrintDecoding();
void menuTFTAnimateFileBrowser(void);
void menuTFTInvalidatePlayArea(void);
void menuTFTDrawLiveCVBars(void);
void menuTFTUpdatePlayState(int vid, int state);
void menuTFTPrintRecordingState(int f0, int f1);

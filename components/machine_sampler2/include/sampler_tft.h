#pragma once
// sampler machine display routines (definitions in sampler_tft.c)
#include <stdbool.h>
#include "cJSON.h"
#include "list.h"
#include "audio_types.h"
#include "menu_types.h"
#include "menu_envelope.h"
#include "menu_playmode.h"

void s2_menuTFTPrintVoiceMenu();
void s2_menuTFTPrintADSRMenu();
void s2_menuTFTPrintPlaymodeMenu();
void s2_menuTFTPrintMatrixMenu();
void s2_menuTFTPrintSlotMenu(const cJSON *slotData, int activeSlot);
void s2_menuTFTPrintBrowseTextMenu();
void s2_menuTFTPrintLoadingTagMenu();
void s2_menuTFTPrintUserFileMenu();
void s2_menuTFTPrintSelectIDMenu();
void s2_menuTFTPrintPresetLayout();
void s2_menuTFTPrintPresetList(cJSON* bank, print_ids_t action);
void s2_menuTFTPrintBankList(list_t* list, print_ids_t action);
void s2_menuTFTSelectVoiceMenu(int, int);
void s2_menuTFTSelectMatrixItem(int active, int select, int column);
void s2_menuTFTSelectPreset(int* activeSlot, cJSON* rootArray, int selected);
void s2_menuTFTSelectPresetBank(int* activeSlot, list_t* list, int selected);
void s2_menuTFTPrintVoiceValues(param_data_t* data, int);
void s2_menuTFTPrintADSRValues(adsr_data_t* data, int);
void s2_menuTFTPrintFilterValues(filter_data_t* data, int);
void s2_menuTFTPrintPlaymodeValues(play_state_data_t* data, int);
void s2_menuTFTPrintMatrixAmount(matrix_ui_row_t* matrix, int);
void s2_menuTFTPrintMatrixDestination(matrix_ui_row_t* matrix, int);
void s2_menuTFTPrintDelayValues(delay_data_t* data, int);
void s2_menuTFTPrintExtInValues(ext_in_data_t* data, int);
void s2_menuTFTPrintPlaymodeIndicators(play_state_data_t* data, int);
void s2_menuTFTInitPlaymodeIndicators();
void s2_menuTFTPrintADSRCurve(adsr_data_t* data);
void s2_menuTFTPrintMatrixRowIndicator(int select, int row);
void s2_menuTFTInitAdsrCurve();
void s2_menuTFTPrintCurrentSettings(param_data_t*);
void s2_menuTFTPrintCurrentPresetSettings(char* title, char* data);
void s2_menuTFTFeedbackMenuItemHSpaced(int *cnt, int* pos, const char** items, const int* n_items, int state);
void s2_menuTFTUpdateProgress(char *text, int progress);
void s2_menuTFTPrintBrowseTagList(list_t*);
int s2_printTags(list_item_t* it);
void s2_menuTFTPrintFileBrowser(list_t* files, int current, bool align_right);
void s2_menuTFTPrintDecoding();
void s2_menuTFTAnimateFileBrowser(void);
void s2_menuTFTInvalidatePlayArea(void);
void s2_menuTFTDrawLiveCVBars(const matrix_ui_row_t *mtx);
void s2_menuTFTUpdatePlayState(int vid, int state);
void s2_menuTFTPrintRecordingState(int f0, int f1);

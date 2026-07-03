#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "audio.h"
#include "menu_types.h"
#include "menu_shapes.h"
#include "list.h"
#include "timer_utils.h"

//Printing menu functions
void menuTFTPrintMenu(const char** items, const int* n_items);
void menuTFTPrintMenuH(const char** items, const int* n_items);
void menuTFTPrintMenuHSpaced(const char** items, const int* n_items);
void menuTFTPrintMainMenus(const char *const *items, int n);
void menuTFTPrintAbout();





void menuTFTPrintSettings(const cJSON *data);



void menuTFTPrintPresetMenu(const char** items, const int* n_items);

void menuTFTPrintInputMenu(char* title);


//Selecting menu item functions
void menuTFTSelectMenuItem(int* activeSlot, int selected, const char** items, const int* n_items);
void menuTFTSelectMenuItemH(int* activeSlot, int selected, const char** items, const int* n_items);
void menuTFTSelectMenuItemHSpaced(int* activeSlot, int selected, const char** items, const int* n_items);
void menuTFTSelectMainMenu(int active, int select, const char *const *items, int n);




//Print parameter values








//Additional UI
void menuTFTPrintTime(int*);
void menuTFTPrintTimezone(const char** items, const int* n_items, int *shift);








//Utility


int menuTFTHighlightNextEl();
int menuTFTHighlightPrevEl();
void menuTFTPrintCharFix(char, int);
void menuTFTPrintChar(char* str, int pos, char c, print_ids_t id);
int menuTFTPrintAllCharSettings(char*);



int printSubStringIfTooWide(char *s, int x, int y, int pos);



void menuTFTPrintRecordIndicator(void);
void menuTFTPrintInputError(char*);
void menuTFTClearListItem(int* activeSlot);
void menuTFTResetTextWrap();

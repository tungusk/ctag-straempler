#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <unistd.h>
#include "esp_log.h"
#include "tft.h"
#include "string_tools.h"
#include "menutft.h"
#include "menutft_utils.h"
#include "list.h"
#include "freertos/timers.h"
#include "strampler_version.h"
#include "recording.h"
#include "wifi.h"

//definitions for element highlighting and selection
int _cur_row = 0, _cur_el = -1;
list_t *_bbox_list = NULL;

//definitions for general menu elements
static int16_t twrap = 0;
static int16_t wrap_pos = 0;



//Menu printing functions
//-------------------------------------------------------------------------------------------------------
void menuTFTPrintMenu(const char** items, const int* n_items){
    int y = 0, x = 4;
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    
    _bg = TFT_BLACK;
    _fg = TFT_LIGHTGREY;
    TFT_fillWindow(_bg);
    int i;
    _cur_row = 0;
    for(i = 0; i < *n_items; i++){
        TFT_print((char *)items[i], x, 3 + (TFT_getfontheight() + 3) *_cur_row);
        _cur_row++;
    }
    TFT_resetclipwin();
}

void menuTFTPrintMenuH(const char** items, const int* n_items){
    int x = 4, w = 0;
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    TFT_fillWindow(TFT_BLACK);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 0;
    for(int i=0; i < *n_items; i++){
        w = TFT_getStringWidth((char*)items[i]);
        TFT_print((char *)items[i], x, 4);
        x += w + 8;
    }
    TFT_resetclipwin();
}

void menuTFTPrintMenuHSpaced(const char** items, const int* n_items){
    int x1 = 0, x2 = 4, w = 0, y = 4;
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    TFT_fillWindow(TFT_BLACK);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 0;
    int x_incr = _width / *n_items;
    for(int i=0; i < *n_items; i++){
        w = TFT_getStringWidth((char *)items[i]);
        int x_offset = (x_incr/2) -  w/2;
        TFT_print((char *)items[i], x1 + x_offset, y);
        x1 += x_incr;
    }
    TFT_resetclipwin();
}

// nav bar background (dark blue)
static const color_t MENUBAR_BG = {10, 18, 56};

void menuTFTPrintMainMenus(const char *const *items, int n){
    int x = 8, w = 0;
    _fg = TFT_WHITE;
	_bg = MENUBAR_BG;
    TFT_setFont(DEFAULT_FONT, NULL);
	TFT_fillRect(0, 0, _width-1, TFT_getfontheight()+8, _bg);
    TFT_X = 0;
    for(int i=0; i < n; i++){
        w = TFT_getStringWidth((char*)items[i]);
        TFT_print((char *)items[i], x, 4);
        x += w + 8;
    }
}

// A labelled affordance, right-justified on a page's title row (e.g. "System"
// on a Setup page, "Settings" on the System page). Selecting it (press) is how
// the hub-less pages reach the next level. Call after the page title.
void menuTFTPrintAffordance(const char *label, int highlighted){
    Font save = cfont;    // save/restore font so a dirty cfont can't garble us
    TFT_resetclipwin();   // draw at the very top, unclipped (else it garbles)
    TFT_setFont(DEFAULT_FONT, NULL);
    int w = TFT_getStringWidth((char*)label);
    int x = _width - w - 8;
    _bg = highlighted ? TFT_CYAN : TFT_BLACK;
    _fg = highlighted ? TFT_BLACK : (color_t){120, 120, 120};
    TFT_fillRect(x - 4, 2, w + 8, TFT_getfontheight() + 3, _bg);
    TFT_print((char*)label, x, 4);
    _bg = TFT_BLACK;
    cfont = save;
}

void menuTFTPrintAbout(){
    // own title + affordance: this page used to inherit whatever the
    // previous page left in the title bar
    _bg = TFT_BLACK;
    TFT_fillRect(0, 0, _width, TFT_getfontheight() + 8, _bg);
    _fg = TFT_WHITE;
    TFT_print("About", 6, 4);
    menuTFTPrintAffordance("Settings", false);   // long-press returns there
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    TFT_fillWindow(TFT_BLACK);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    int fh = TFT_getfontheight() + 2;            // single-spaced (was doubled)
    int y = 4;
    TFT_print("Freesound Sampler Version:", 4, y); y += fh;
    TFT_print(STRAMPLER_FW_VERSION, 4, y);         y += fh + 4;
    TFT_print("Created by:", 4, y);                y += fh;
    TFT_print("Niklas Wantrupp", 4, y);            y += fh;
    TFT_print("Phillip Lamp", 4, y);               y += fh;
    TFT_print("Robert Manzke", 4, y);              y += fh + 4;
    TFT_print("v0.9 contributors:", 4, y);         y += fh;
    TFT_print("Arlo Fishman", 4, y);               y += fh;
    TFT_print("Claude (Anthropic)", 4, y);
    TFT_resetclipwin();   // clip is shared global state — leaking it garbles the title-bar affordance
}






void menuTFTPrintSettings(const cJSON *data){
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height); 
    const char* hidden_items[] = {"passwd", "apikey"};
    const int n_hidden_items = sizeof(hidden_items)/sizeof(int);
    _bg = TFT_BLACK;
    _fg = TFT_WHITE;
    char buf[33];
    int x = 4, cnt = 0;
    cJSON *val = NULL;
    if(data != NULL){
        _cur_row = 0;
        val = cJSON_GetObjectItemCaseSensitive(data, "ssid");
        TFT_print(val->valuestring, x + _width/2, 3 + (TFT_getfontheight() + 3) *_cur_row);
        _cur_row++;
        for(int i = 0; i < n_hidden_items; i++)
        {
            val = cJSON_GetObjectItemCaseSensitive(data, hidden_items[i]);
            strncpy(buf, val->valuestring, 32);
            hideString(buf, 32, 22);
            TFT_print(buf, x + _width/2, 3 + (TFT_getfontheight() + 3) *_cur_row);
            _cur_row++;
        }
    }
    TFT_resetclipwin();
}







void menuTFTPrintIP(const char** items, const int* n_items){
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    _bg = TFT_BLACK;
    _fg = TFT_WHITE;
    char ip[20];
    wifiGetIPString(ip, sizeof(ip));
    int x = 4, row = 0;
    for(int i = 0; i < *n_items; i++){
        if(strcasecmp(items[i], "IP") == 0) row = i;
    }
    TFT_print(ip, x + _width/2, 3 + (TFT_getfontheight() + 3) * row);
    TFT_resetclipwin();
}

void menuTFTPrintInputMenu(char* title){
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    TFT_fillWindow(TFT_BLACK);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 3;
    TFT_print(title, TFT_X, 4);
    TFT_resetclipwin();
}


//-------------------------------------------------------------------------------------------------------




//Selecting menu item functions
//-------------------------------------------------------------------------------------------------------
void menuTFTSelectMenuItem(int* activeSlot, int selected, const char** items, const int* n_items){
    int h = 0, y = 0;
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 0;
    h = TFT_getfontheight();
    for(int i = 0;i < *n_items; i++){
        _bg = TFT_BLACK;
        if(i== *activeSlot){
            if(selected == 1){
                _bg = TFT_RED;
            }else{
                _bg = TFT_CYAN;
            }
        }
        TFT_drawRect(2, y, TFT_getStringWidth((char*)items[i]) + 4, h + 3 , _bg);
        y += h + 3;
    }
    TFT_resetclipwin();   // clip is shared global state — leaking it garbles the title-bar affordance
}

void menuTFTSelectMenuItemH(int* activeSlot, int selected, const char** items, const int* n_items){
    int x = 0, w = 0;
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 0;
    for(int i=0;i<*n_items;i++){
        _bg = TFT_BLACK;
        if(i== *activeSlot){
            if(selected == 1){
                _bg = TFT_RED;
            }else{
                _bg = TFT_CYAN;
            }
        }
        w = TFT_getStringWidth((char*)items[i]);
        TFT_drawRect(x, 0, w+8, TFT_getfontheight()+8, _bg);
        x += w + 8;
    }
    TFT_resetclipwin();
}

void menuTFTSelectMenuItemHSpaced(int* activeSlot, int selected, const char** items, const int* n_items){
    int x1 = 0, x2 = 0, w = 0, x_incr = _width / *n_items, x_offset = 0;
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 0;
    //ESP_LOGI("UI", "Slot: %d, Selected: %d", *activeSlot, selected);
    for(int i=0;i<*n_items;i++){
        _bg = TFT_BLACK;
        if(i== *activeSlot){
            if(selected == 1){
                _bg = TFT_RED;
            }else{
                _bg = TFT_CYAN;
            }
        }
        w = TFT_getStringWidth((char*)items[i]);
        x_offset = (x_incr/2) -  (w/2) - 4;
        TFT_drawRect(x1 + x_offset, 0, w+8, TFT_getfontheight()+8, _bg);
        x1 += x_incr;
    }
    TFT_resetclipwin();
}

void menuTFTSelectMainMenu(int active, int select, const char *const *items, int n){
    int x = 4, w = 0;
    TFT_resetclipwin();
    for(int i=0;i<n;i++){
        _bg = MENUBAR_BG;
        if(i==active){
            if(select == 1){
                _bg = TFT_RED;
            }else{
                _bg = TFT_CYAN;
            }
        }
        w = TFT_getStringWidth((char*)items[i]);
        TFT_drawRect(x, 0, w+8, TFT_getfontheight()+8, _bg);
        x += w + 8;
    }
}




//-------------------------------------------------------------------------------------------------------




//Print parameter values
//-------------------------------------------------------------------------------------------------------







//-------------------------------------------------------------------------------------------------------




//Additional UI
//-------------------------------------------------------------------------------------------------------
void menuTFTPrintTime(int *shift){
    time_t time_now;
    struct tm* tm_info;
    char s[10];
    time(&time_now);
    tm_info = localtime(&time_now);
    tm_info->tm_hour += *shift;
    mktime(tm_info);
    //ESP_LOGI("MENU", "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    snprintf(s, 10, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    TFT_saveClipWin();
    TFT_resetclipwin();
    _bg = MENUBAR_BG;
    _fg = TFT_WHITE;
    TFT_print(s, _width - TFT_getStringWidth(s) - 4, 4);
    TFT_restoreClipWin();
}

void menuTFTPrintRecordIndicator(void) {
    bool active = recording_is_active();

    TFT_saveClipWin();
    TFT_resetclipwin();

    // Clear the right side of the nav bar (where clock was)
    int iw = 64;
    TFT_fillRect(_width - iw - 2, 1, iw, TFT_getfontheight() + 6, MENUBAR_BG);

    char label[10] = "";
    if (active) {
        snprintf(label, sizeof(label), "REC");
        _fg = TFT_RED;
    }
    // T0/T1 ARM labels hidden (Arlo 2026-07-05): stale 0-based naming and
    // visual noise; arm state still shows in the transport bar colors
    _bg = MENUBAR_BG;

    if (label[0]) {
        TFT_print(label, _width - TFT_getStringWidth(label) - 4, 4);
    }

    TFT_restoreClipWin();
}

void menuTFTPrintTimezone(const char** items, const int* n_items, int *shift){
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    _bg = TFT_BLACK;
    _fg = TFT_WHITE;
    char buf[32];
    int x = 4, cnt = 0;
    _cur_row = 0;
    for(int i = 0; i < *n_items; i++)
    {
        if(strcasecmp(items[i],"Timezone") == 0) _cur_row = i;
    }
    menuTFTFlushValue(abs(*shift), _cur_row, negpos_conditions, &negpos_cond_size, &_bg);
    (*shift >= 0) ? sprintf(buf, "CET+%d",*shift) : sprintf(buf, "CET%d",*shift);
    TFT_print(buf, x + _width/2, 3 + (TFT_getfontheight() + 3) *_cur_row);
    TFT_resetclipwin();
}

void menuTFTPrintRemote(const char** items, const int* n_items, int *on){
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    _bg = TFT_BLACK;
    _fg = TFT_WHITE;
    int x = 4, row = 0;
    for(int i = 0; i < *n_items; i++){
        if(strcasecmp(items[i], "Remote") == 0) row = i;
    }
    int y = 3 + (TFT_getfontheight() + 3) * row;
    TFT_fillRect(x + _width/2, y, _width/2 - x - 4, TFT_getfontheight() + 2, _bg);
    TFT_print(*on ? "ON" : "OFF", x + _width/2, y);
    TFT_resetclipwin();
}








//-------------------------------------------------------------------------------------------------------




//Utility
//-------------------------------------------------------------------------------------------------------


int menuTFTHighlightNextEl(){
    _bg = TFT_BLACK;
    bbox_t *bbox = list_get_item(_bbox_list, _cur_el)->value;
    TFT_drawRect(bbox->x, bbox->y, bbox->w, bbox->h, _bg);
    _cur_el++;
    if(_cur_el >= _bbox_list->count) _cur_el = 0;
    bbox = list_get_item(_bbox_list, _cur_el)->value;
    _fg = TFT_CYAN;
    TFT_drawRect(bbox->x, bbox->y, bbox->w, bbox->h, _fg);
    return _cur_el;
}

int menuTFTHighlightPrevEl(){
    _bg = TFT_BLACK;
    bbox_t *bbox = list_get_item(_bbox_list, _cur_el)->value;
    TFT_drawRect(bbox->x, bbox->y, bbox->w, bbox->h, _bg);
    _cur_el--;
    if(_cur_el < 0) _cur_el = _bbox_list->count - 1;
    bbox = list_get_item(_bbox_list, _cur_el)->value;
    _fg = TFT_CYAN;
    TFT_drawRect(bbox->x, bbox->y, bbox->w, bbox->h, _fg);
    return _cur_el;
}

void menuTFTPrintCharFix(char c, int pos){
    //printing with fixed space of 14px
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    TFT_X = 3 + pos * 14;
    char s[2] = "\0";
    s[0] = c;
    int y = 8 + TFT_getfontheight();
    Font f = cfont;

    TFT_setFont(UBUNTU16_FONT, NULL);
    TFT_fillRect(TFT_X, y, 14, TFT_getfontheight(), _bg);                       //flush previous char
    TFT_print(s, TFT_X, y);                                                     //print new char
    TFT_fillRect(TFT_X, y, 14, TFT_getfontheight(), _bg);    
    cfont = f;
}

void menuTFTPrintChar(char* str, int pos, char c, print_ids_t id){
    _fg = TFT_WHITE;
    _bg = TFT_BLACK;
    char s[2] = "\0";       
    int x = 0, w = 20, y =  8 + TFT_getfontheight();
    //Check if char should be printed in upper case
    s[0] = (id == PRINT_UPPER && isalpha(c) != 0) ? toupper(c) : c;
    //Set font
    Font f = cfont;
    TFT_setFont(UBUNTU16_FONT, NULL);
    //Get x position for char
    x = menuTFTGetCharPos(str, pos);
    //Check if text is wrapped
    if(x >= _width - 12){
        if(!twrap){
            twrap = 1;
            wrap_pos = x;
        }
        TFT_X = 3 + x - wrap_pos;
        y *= 2;
    }else{
        TFT_X = x;
        TFT_fillRect(0, y*2, w, TFT_getfontheight(), _bg); 
        twrap = wrap_pos = 0;
    }
    //print
    TFT_fillRect(TFT_X, y, w, TFT_getfontheight(), _bg);    //clear previous 
    TFT_print(s, TFT_X, y);
    TFT_fillRect(TFT_X + 2, y, w, TFT_getfontheight(), _bg);    //clear everything to right of char
    cfont = f;
}

int menuTFTPrintAllCharSettings(char* str){
    int sz = strlen(str);
    int i = 0;
    for(; i < sz; i++)
        menuTFTPrintChar(str, i, str[i], PRINT_NORM);
    return i;
}





int printSubStringIfTooWide(char *s, int x, int y, int pos){
    if(strlen(s) > pos){
        if(x + TFT_getStringWidth(&s[pos]) > 317){
            TFT_fillRect(x, y, 317 - x, (TFT_getfontheight() + 3), _bg);
            TFT_print(&s[pos], x, y);
            return 1;
        }
    }
    return 0;
}







void menuTFTPrintInputError(char* s){
    TFT_setclipwin(0,TFT_getfontheight()+9, _width-1, _height);
    char str[32];
    sprintf(str, "%s", s);
    _fg = TFT_RED;
    _bg = TFT_BLACK;
    TFT_print(str, CENTER, CENTER);
    TFT_resetclipwin();
}

void menuTFTClearListItem(int* activeSlot){
    TFT_setclipwin(0, 3* TFT_getfontheight() + 32, _width-1, _height);
    _bg = TFT_BLACK;
    int x = 0;
    int x_incr = _width / 3;
    int y_start = 0;
    int y = 8 + TFT_getfontheight();
    int y_draw = 0;

    if(*activeSlot < 10)y_draw = y_start + 3 + (TFT_getfontheight() + 3) * (*activeSlot);
    if(*activeSlot >= 10 && *activeSlot < 20){
        y_draw = y_start + 3 + (TFT_getfontheight() + 3) * ((*activeSlot) - 10);
        x = x_incr;
    }
    if(*activeSlot >= 20 && *activeSlot < 30){
        y_draw = y_start + 3 + (TFT_getfontheight() + 3) * ((*activeSlot) - 20);
        x = x_incr*2;
    }            
        
    TFT_fillRect(x, y_draw - 4, x_incr-1, TFT_getfontheight() + 4, _bg);
    TFT_resetclipwin();
}

void menuTFTResetTextWrap(){
    twrap = 0;
    wrap_pos = 0;
}
//-------------------------------------------------------------------------------------------------------
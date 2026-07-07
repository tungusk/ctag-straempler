#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "menu.h"
#include "tft.h"
#include "freesound.h"
#include "list.h"
#include "mp3.h"
#include "menusys.h"
#include "fileio.h"
#include "storage.h"
#include "ui_events.h"
#include "esp_log.h"
#include "audio.h"
#include "menutft.h"
#include "recording.h"
#include "menu_types.h"
#include "menutft_utils.h"
#include "gpio.h"
#include "audio.h"
#include "wifi.h"
#include "esp_wifi.h"
#include "timer_utils.h"
#include "esp_timer.h"
#include "rest-api.h"
#include "menu_config.h"
#include "menu_nav.h"
#include "machine.h"

#define N_MAIN_MENUS 5


static void autosave_kick(void);
static xQueueHandle s_ev_queue = NULL;

// core menu labels (machine-independent pages)
static const char* more_menus[] = {"Machine", "Settings", "About"};
static const int n_more_menus = 3;
static const char* settings_menus[] = {"SSID", "Password", "Api Key", "Timezone", "Remote", "IP"};
static const int n_settings_menus = 6;

static void incSettingsItem(int *tz, int index){
    if (index == SID_TIMEZONE) { if(*tz + 1 >= 12) *tz = 12; else (*tz)++; }
}
static void decSettingsItem(int *tz, int index){
    if (index == SID_TIMEZONE) { if(*tz - 1 <= -12) *tz = -12; else (*tz)--; }
}
static int tz_shift = 0;
menusys_t *_ms = NULL;
// data which needs to be passed from one menu state to another
// should be only used to pass data between defined state transitions
void *_state_data = NULL;
void *_state_voice = NULL;
void *_state_json = NULL;
void *_fb_state = NULL;

// handler has format caller_id, caller_name, caller_item data, event, event data
static int timer_handler(int it_id, int event, void* event_data){
    menuTFTPrintRecordIndicator();
    if (it_id == M_MAIN) {
        const machine_t *m = machine_active();
        if (m && m->ui && m->ui->main_event)
            m->ui->main_event(EV_TIMER_REPEATING_SLOW, event_data);
    }
    return 0; // remain in current menu
}

// main menu = active machine's entries + core "System". Rebuilt on machine bind.
static const char *s_main_labels[9] = {"System"};
static int s_main_targets[9] = {M_MORE};
static int s_n_main = 1;

static const machine_ui_t *machine_ui(void){
    const machine_t *m = machine_active();
    return m ? m->ui : NULL;
}

// sampler main-screen live area (arm cycling + live displays).
// M0c-1: still in this file; moves out with the rest of the sampler UI.
// main-menu selection index: file-scope so a machine rebind can reset it —
// the entry count changes across machines and a stale index points nowhere
static int s_main_menu_pos = 0;

static int main_menu_def_handler(int it_id, int event, void* event_data){
    const machine_ui_t *mui = machine_ui();

    switch(event){
        case EV_ENTERED_MENU:
            // repaint the whole bar (labels included): a machine's full-screen
            // page may have cleared it, and SelectMainMenu only draws the
            // highlight boxes, not the label text
            menuTFTPrintMainMenus(s_main_labels, s_n_main);
            menuTFTSelectMainMenu(s_main_menu_pos, 0, s_main_labels, s_n_main);
            if (mui && mui->main_event) mui->main_event(event, event_data);
            break;
        case EV_FWD:
            s_main_menu_pos++;
            if(s_main_menu_pos >= s_n_main) s_main_menu_pos = 0;
            menuTFTSelectMainMenu(s_main_menu_pos, 0, s_main_labels, s_n_main);
            break;
        case EV_BWD:
            s_main_menu_pos--;
            if(s_main_menu_pos < 0) s_main_menu_pos = s_n_main - 1;
            menuTFTSelectMainMenu(s_main_menu_pos, 0, s_main_labels, s_n_main);
            break;
        case EV_LONG_PRESS:
            menuTFTSelectMainMenu(s_main_menu_pos, 1, s_main_labels, s_n_main);
            return s_main_targets[s_main_menu_pos];
            break;
        default:
            if (mui && mui->main_event) return mui->main_event(event, event_data);
            break;
    }

    return 0; // remain in current menu
}

static int about_def_handler(int it_id, int event, void* event_data){
    switch(event){
        case EV_ENTERED_MENU:
            menuTFTPrintAbout();
            //unmountSDStorage();
            break;
        case EV_LONG_PRESS:
            //mountSDStorage();
            menuTFTFlushMenuDataRect();
            return M_MORE;
            break;
        default:
            break;
    }

    return 0; // remain in current menu
}

static int more_def_handler(int it_id, int event, void* event_data){
    const int menu_states[] = {M_MACHINE_SEL, M_SETTINGS, M_ABOUT};
    const int states = sizeof(menu_states)/sizeof(int);
    static int menu_state_current = 0;

    switch(event){
        case EV_ENTERED_MENU:
            menuTFTPrintMenu(more_menus, &n_more_menus);
            menuTFTSelectMenuItem(&menu_state_current, 0, more_menus, &n_more_menus);
            break;
        case EV_FWD:
            menu_state_current++;
            if(menu_state_current >= states) menu_state_current = 0;
            menuTFTSelectMenuItem(&menu_state_current, 0, more_menus, &n_more_menus);
            break;
        case EV_BWD:
            menu_state_current--;
            if(menu_state_current < 0) menu_state_current = states - 1;
            menuTFTSelectMenuItem(&menu_state_current, 0, more_menus, &n_more_menus);
            break;
        case EV_SHORT_PRESS:
            return menu_states[menu_state_current];
            break;
        case EV_LONG_PRESS:
            menuTFTFlushMenuDataRect();
            return M_MAIN;
            break;
        default:
            break;
    }
    
    return 0; // remain in current menu
}

static void autosave_now(void);

// switch the active machine: capture the outgoing machine's state, swap
// (machine_core mutes audio around the stop/start), persist the choice, and
// queue the UI rebind — menuMachineBindNow rebuilds menusys with the new
// machine's pages and re-enters M_MAIN on the next event-loop pass
static void menuSwitchMachine(const machine_t *m){
    autosave_now();
    if(machine_activate(m) != ESP_OK){
        ESP_LOGE("UI", "machine %s failed to start", m->name);
        return;
    }
    configSetStringSetting("machine", m->name);
    menuBindMachineUI();
}

static int machine_sel_def_handler(int it_id, int event, void* event_data){
    static const char *names[16];
    static const machine_t *machines[16];   // visible-position -> machine (Stub filtered out)
    static int n = 0;
    static int pos = 0;

    switch(event){
        case EV_ENTERED_MENU:
            n = 0;
            pos = 0;
            for(int i = 0; machine_registry[i] != NULL && n < 16; i++){
                if(strcmp(machine_registry[i]->name, "Stub") == 0) continue; // hidden fallback
                if(machine_registry[i] == machine_active()) pos = n;
                machines[n] = machine_registry[i];
                names[n] = machine_registry[i]->name;
                n++;
            }
            menuTFTPrintMenu(names, &n);
            menuTFTSelectMenuItem(&pos, 0, names, &n);
            break;
        case EV_FWD:
            pos++;
            if(pos >= n) pos = 0;
            menuTFTSelectMenuItem(&pos, 0, names, &n);
            break;
        case EV_BWD:
            pos--;
            if(pos < 0) pos = n - 1;
            menuTFTSelectMenuItem(&pos, 0, names, &n);
            break;
        case EV_SHORT_PRESS:
            if(machines[pos] != machine_active())
                menuSwitchMachine(machines[pos]);
            return 0; // queued EV_MACHINE_BIND re-enters M_MAIN itself
        case EV_LONG_PRESS:
            menuTFTFlushMenuDataRect();
            return M_MORE;
        default:
            break;
    }
    return 0; // remain in current menu
}

static int settings_def_handler(int it_id, int event, void* event_data){
    const int menu_items[] = {SID_WIFI_SSID, SID_WIFI_PASSWD, SID_APIKEY, SID_TIMEZONE, SID_REMOTE};
    const int items = sizeof(menu_items)/sizeof(int);
    static int menu_pos = 0, selected = 0;
    static int remote_on = 1;
    static cJSON *cfgData = NULL, *settings = NULL;
    switch(event){
        case EV_ENTERED_MENU:
            selected = 0;
            menuTFTPrintMenu(settings_menus, &n_settings_menus);
            menuTFTSelectMenuItem(&menu_pos, 0, settings_menus, &n_settings_menus);
            if(_state_json != NULL)cfgData = (cJSON*) _state_json;
            else cfgData = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
            if(cfgData != NULL){
                settings = cJSON_GetObjectItemCaseSensitive(cfgData, "settings");
                if(settings != NULL)menuTFTPrintSettings(settings);
                remote_on = 1;
                if(settings != NULL){
                    cJSON *r = cJSON_GetObjectItemCaseSensitive(settings, "remote");
                    if(r != NULL && cJSON_IsNumber(r)) remote_on = r->valueint ? 1 : 0;
                }
                menuTFTPrintTimezone(settings_menus, &n_settings_menus, &tz_shift);
                menuTFTPrintRemote(settings_menus, &n_settings_menus, &remote_on);
                menuTFTPrintIP(settings_menus, &n_settings_menus);
            }else ESP_LOGE("UI", "couldn't fetch cfgData from state or file");
            break;
        case EV_FWD:
            if(!selected){
                menu_pos++;
                if(menu_pos >= items) menu_pos = 0;
                menuTFTSelectMenuItem(&menu_pos, 0, settings_menus, &n_settings_menus);
            }else if(menu_items[menu_pos] == SID_REMOTE){
                remote_on = !remote_on;
                menuTFTPrintRemote(settings_menus, &n_settings_menus, &remote_on);
            }else{
                incSettingsItem(&tz_shift, menu_items[menu_pos]);
                menuTFTPrintTimezone(settings_menus, &n_settings_menus, &tz_shift);
            }
            break;
        case EV_BWD:
            if(!selected){
                menu_pos--;
                if(menu_pos < 0) menu_pos = items - 1;
                menuTFTSelectMenuItem(&menu_pos, 0, settings_menus, &n_settings_menus);
            }else if(menu_items[menu_pos] == SID_REMOTE){
                remote_on = !remote_on;
                menuTFTPrintRemote(settings_menus, &n_settings_menus, &remote_on);
            }else{
                decSettingsItem(&tz_shift, menu_items[menu_pos]);
                menuTFTPrintTimezone(settings_menus, &n_settings_menus, &tz_shift);
            }
            break;
        case EV_SHORT_PRESS:
            if(menu_items[menu_pos] != SID_TIMEZONE && menu_items[menu_pos] != SID_REMOTE){
                _state_json = (void*) cfgData;
                _state_data = (void*) &menu_pos;
                return M_SETTINGS_INPUT;
            }else{
                selected = !selected;
                menuTFTSelectMenuItem(&menu_pos, selected, settings_menus, &n_settings_menus);
            }
            break;
        case EV_LONG_PRESS: ;
            int wifiChanged = wifiSettingsChanged(settings);
            //replace tz_shift value
            cJSON_ReplaceItemInObjectCaseSensitive(settings, "tz_shift", cJSON_CreateNumber(tz_shift));
            //persist + apply the teleremote toggle
            cJSON_DeleteItemFromObjectCaseSensitive(settings, "remote");
            cJSON_AddNumberToObject(settings, "remote", remote_on);
            rest_remote_enable(remote_on);
            //save current settings on menu exit
            writeJSONFile("/sdcard/CONFIG.jsn", cJSON_Print(cfgData));
            //set token 
            char* token = cJSON_GetObjectItem(settings, "apikey")->valuestring;
            freesoundSetToken(token);
            //if wifi settings changed reconnect wifi with new config
            if(wifiChanged){
                ESP_LOGI("UI", "Wifi settings have changed. Reconnecting...");
                char *ssid = cJSON_GetObjectItem(settings, "ssid")->valuestring;
                char *passwd = cJSON_GetObjectItem(settings, "passwd")->valuestring;
                wifi_config_t wifi_config;
                memset(&wifi_config, 0, sizeof(wifi_config));
                strcpy((char*) wifi_config.sta.ssid, ssid);
                strcpy((char*) wifi_config.sta.password, passwd);
                restartWifi(&wifi_config);
            }
            menuTFTFlushMenuDataRect();
            _state_data = NULL;
            _state_json = NULL;
            menu_pos = 0;
            cJSON_Delete(cfgData);
            return M_MORE;
            break;
        default:
            break;
    }

    return 0; // remain in current menu
}

static int settings_input_def_handler(int it_id, int event, void* event_data){
    const char *c_list = "=0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_ !?<^";
    static int pos = 0, c = 1, menu_pos = 0;
    static char input[48];
    char *buf;
    char title[32];
    static cJSON *root = NULL, *settings = NULL, *val = NULL;

    switch(event){
        case EV_ENTERED_MENU:
            bzero(input, 48);
            bzero(title, 32);
            pos = 0;
            c = 41;
            menu_pos = 0;
            menuTFTResetTextWrap();
            //Get settings object from _state_json & menu_pos from _state_data
            if(_state_json != NULL){
                root = (cJSON*) _state_json;
                if(root != NULL)settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
                menu_pos = *((int*) _state_data);
                if(settings != NULL)sprintf(title, "Enter %s:", settings_menus[menu_pos]);
                menuTFTPrintInputMenu(title);
                val = cJSON_GetArrayItem(settings, menu_pos);
                //print current valuestring, copy to input buffer and increase pos to stringlength
                if(cJSON_IsString(val)){
                    pos = menuTFTPrintAllCharSettings(val->valuestring);
                    strcpy(input, val->valuestring);
                }
            }else ESP_LOGE("PRESET", "_state_json is NULL");
        case EV_FWD:
            c++;
            if(c>69) c = 69;
            menuTFTPrintChar(input, pos, c_list[c], PRINT_NORM);
            break;
        case EV_BWD:
            c--;
            if(c<0)c=0;
            menuTFTPrintChar(input, pos, c_list[c], PRINT_NORM);
            break;
        case EV_SHORT_PRESS:
            switch(c_list[c]){
                case '^':
                    _state_data = NULL;
                    menuTFTFlushMenuDataRect();
                    return M_SETTINGS;
                    break;
                case '<':
                    input[pos] = '\0';
                    pos--;
                    if(pos<0)pos=0;
                    menuTFTPrintChar(input, pos, c_list[c], PRINT_NORM);
                    break;
                case '=':
                    if(pos == 0) break;
                    input[pos] = '\0';
                    buf = calloc(strlen(input) + 1, 1);
                    strcpy(buf, input);
                    cJSON* val = cJSON_GetArrayItem(settings, menu_pos);
                    cJSON_ReplaceItemInObjectCaseSensitive(settings, val->string, cJSON_CreateString(buf));
                    _state_data = NULL;
                    pos = 0;
                    return M_SETTINGS;
                    break;
                default:
                    input[pos] = c_list[c];
                    pos++;
                    if(menu_pos == 2){
                        if(pos>42)pos=42;
                    }else{
                        if(pos>32)pos=32;
                    }
                    menuTFTPrintChar(input, pos, c_list[c], PRINT_NORM);
                    break;
            }
            break;
        case EV_LONG_PRESS:
            //print current char
            if(c_list[c] == '^' || c_list[c] == '<') break;
            input[pos] = toupper(c_list[c]);
            menuTFTPrintChar(input, pos, c_list[c], PRINT_UPPER);
            pos++;
            if(menu_pos == 2){
                if(pos>42)pos=42;
            }else{
                if(pos>32)pos=32;
            }
            menuTFTPrintChar(input, pos, c_list[c], PRINT_NORM);
            break;
        default:
            break;
    }

    return 0;
}

static void autosave_now(void);
static void menuMachineBindNow(void);

void menuProcessEvent(int ev, void * ev_data){
    // autosave runs here (UI event loop task) rather than in any menu handler
    if(ev == EV_AUTOSAVE){
        autosave_now();
        return;
    }
    if(ev == EV_MACHINE_BIND){
        menuMachineBindNow();
        return;
    }
    // teleremote machine switch: runs here (UI task) so the autosave/activate/
    // rebind sequence is identical to a front-panel switch
    if(ev == EV_REMOTE_MACHINE){
        char *name = (char*) ev_data;
        const machine_t *m = name ? machine_by_name(name) : NULL;
        if(m != NULL && strcmp(m->name, "Stub") != 0 && m != machine_active())
            menuSwitchMachine(m);
        free(name);
        return;
    }
    // teleremote settings apply: same JSON shape as the autosave state, fed
    // through preset_load on the UI task, then persisted by the autosave
    if(ev == EV_REMOTE_PRESET){
        char *js = (char*) ev_data;
        const machine_t *m = machine_active();
        if(js != NULL && m != NULL && m->preset_load != NULL){
            cJSON *node = cJSON_Parse(js);
            if(node != NULL){
                m->preset_load(node);
                cJSON_Delete(node);
                autosave_kick();
            }
        }
        free(js);
        return;
    }
    // any user input may change a parameter — (re)arm the debounced state save,
    // so edits are captured even if power is cut while still inside a submenu
    if(ev == EV_FWD || ev == EV_BWD || ev == EV_SHORT_PRESS || ev == EV_LONG_PRESS)
        autosave_kick();
    menusys_process_ev(_ms, ev, ev_data);
}

static esp_timer_handle_t s_autosave_timer = NULL;

// esp_timer task stack (2048) is too small for cJSON + SD writes; defer to the UI event loop
static void autosave_cb(void *arg) {
    ui_ev_ts_t ev = { .event = EV_AUTOSAVE, .event_data = NULL };
    xQueueSend(s_ev_queue, &ev, 0);
}

// AUTOSAVE.JSN keeps each machine's state under its own name key, so every
// machine remembers its settings independently across switches:
//   { "Sampler": {...}, "Looper": {...}, "Slicer": {...} }
static void autosave_now(void) {
    const machine_t *m = machine_active();
    if (!m || !m->preset_save) return;
    cJSON *node = m->preset_save();
    if (!node) return;
    cJSON *root = readJSONFileAsCJSON("/sdcard/AUTOSAVE.JSN");
    if (!root) root = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(root, m->name);   // replace this machine's entry
    cJSON_AddItemToObject(root, m->name, node);               // root now owns node
    char *out = cJSON_PrintUnformatted(root);
    if (out) { writeJSONFile("/sdcard/AUTOSAVE.JSN", out); free(out); }
    cJSON_Delete(root);
    ESP_LOGI("AUTOSAVE", "State saved (%s)", m->name);
}

static void autosave_kick(void) {
    if (!s_autosave_timer) return;
    esp_timer_stop(s_autosave_timer);
    esp_timer_start_once(s_autosave_timer, 2000000);
}

// called by the app after machine_activate(). The actual bind (page
// registration, machine preset boot-load, full main-screen draw) runs on the
// UI event task — app_main's 4KB stack is too small for the cJSON + TFT work
// (suspected cause of the intermittent boot loop seen on 2026-07-03).
void menuBindMachineUI(void){
    ui_ev_ts_t ev = { .event = EV_MACHINE_BIND, .event_data = NULL };
    xQueueSend(s_ev_queue, &ev, portMAX_DELAY);
}

// core pages, machine-agnostic. Rebuilt (together with the machine's pages)
// on every bind: menusys has no item removal, so a fresh instance is the only
// way to drop the outgoing machine's pages and avoid duplicate ids.
static void register_core_pages(void){
    _ms = menusys_create();
    menusys_new_item(_ms, M_MAIN);
    menusys_item_set_default_cb(_ms, M_MAIN, main_menu_def_handler);

    menusys_new_item(_ms, M_MORE);
    menusys_item_set_default_cb(_ms, M_MORE, more_def_handler);
        menusys_new_item(_ms, M_MACHINE_SEL);
        menusys_item_set_default_cb(_ms, M_MACHINE_SEL, machine_sel_def_handler);
        menusys_new_item(_ms, M_ABOUT);
        menusys_item_set_default_cb(_ms, M_ABOUT, about_def_handler);
        menusys_new_item(_ms, M_SETTINGS);
        menusys_item_set_default_cb(_ms, M_SETTINGS, settings_def_handler);
            menusys_new_item(_ms, M_SETTINGS_INPUT);
            menusys_item_set_default_cb(_ms, M_SETTINGS_INPUT, settings_input_def_handler);

    menusys_all_set_ev_cb(_ms, EV_TIMER_REPEATING_SLOW, timer_handler);
}

static void menuMachineBindNow(void){
    if(_ms) menusys_free(_ms);
    register_core_pages();
    s_main_menu_pos = 0;   // entry count differs per machine; stale index is invalid

    const machine_ui_t *mui = machine_ui();
    int n = 0;
    if (mui) {
        if (mui->register_pages) mui->register_pages(_ms);
        for (; n < mui->n_main && n < 8; n++) {
            s_main_labels[n] = mui->main_items[n];
            s_main_targets[n] = mui->main_targets[n];
        }
    }
    s_main_labels[n] = "System";
    s_main_targets[n] = M_MORE;
    s_n_main = n + 1;

    // per-machine state restore: AUTOSAVE.JSN keys each machine's state under
    // its name; a missing entry means "load your defaults" (NULL)
    const machine_t *m = machine_active();
    if (m && m->preset_load) {
        cJSON *root = readJSONFileAsCJSON("/sdcard/AUTOSAVE.JSN");
        cJSON *node = root ? cJSON_GetObjectItemCaseSensitive(root, m->name) : NULL;
        m->preset_load(node);
        cJSON_Delete(root);
    }

    // a machine switch redraws over whatever page was on screen, and menu
    // pages leave the TFT clip window set below the menu bar — reset and
    // clear so the rebuilt UI starts from a clean slate like boot does
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    menuTFTPrintMainMenus(s_main_labels, s_n_main);
    menusys_set_active_item(_ms, M_MAIN);
    menuProcessEvent(EV_ENTERED_MENU, NULL);
}

void initMenu(xQueueHandle ev_queue){
    s_ev_queue = ev_queue;
    recording_set_arm_monitor(configGetIntSetting("rec_monitor", 1) != 0);
    initTimeshift(&tz_shift);
    TFT_fillScreen(TFT_BLACK);
    TFT_resetclipwin();
    menuTFTPrintMainMenus(s_main_labels, s_n_main);

    esp_timer_create_args_t autosave_args = { .callback = autosave_cb, .name = "autosave" };
    esp_timer_create(&autosave_args, &s_autosave_timer);

    register_core_pages();
    menusys_set_active_item(_ms, M_MAIN);
}


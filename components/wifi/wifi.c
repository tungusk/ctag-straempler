#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_attr.h"
#include <unistd.h>
#include <stdio.h>
#include "tcpip_adapter.h"
#include "lwip/apps/sntp.h"
#include "mdns.h"
#include "esp_log.h"
#include "wifi.h"
#include "espnow_probe.h"
#include "fileio.h"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;
/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;
static const char *TAG = "WIFI";
static int wifi_ap_mode = 0;
// The PHY-level cap (sdkconfig CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER=10) bounds
// everything including boot-time RF calibration — the burst that collapses the
// rail with the antenna attached. This runtime value only lowers power further
// (CONFIG.JSN "txpwr", 8..84 quarter-dBm, or POST /settings); 0 = PHY cap only.
static int s_txpwr = 0;
static SemaphoreHandle_t s_wifi_mutex = NULL; // serializes restartWifi callers

// Some units run without the WiFi antenna attached; TX bursts at full power can
// brown out the supply. Optional CONFIG.JSN settings field "txpwr" caps it.
static void apply_tx_power(void);

void wifiApplyTxPower(int quarter_dbm){
    if (quarter_dbm < 8 || quarter_dbm > 84) return;
    s_txpwr = quarter_dbm;
    esp_wifi_set_max_tx_power((int8_t)s_txpwr);
    ESP_LOGI(TAG, "TX power capped at %.2f dBm", s_txpwr * 0.25);
}

static void apply_tx_power(void){
    if (s_txpwr) esp_wifi_set_max_tx_power((int8_t)s_txpwr);
}

static struct tm* tm_info;
static time_t time_now;

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static int obtain_time(void)
{
	int res = 1;
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, pdMS_TO_TICKS(30000));

    initialize_sntp();

    // wait for time to be set
    int retry = 0;
    const int retry_count = 20;

    time(&time_now);
	tm_info = localtime(&time_now);

    while(tm_info->tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
		vTaskDelay(500 / portTICK_RATE_MS);
        time(&time_now);
    	tm_info = localtime(&time_now);
    }
    if (tm_info->tm_year < (2016 - 1900)) {
    	ESP_LOGI(TAG, "System time NOT set.");
    	res = 0;
    }
    else {
    	ESP_LOGI(TAG, "System time is set.");
    }

    setenv("TZ", "CET", 1);
    tzset();

    //ESP_ERROR_CHECK( esp_wifi_stop() );
    return res;
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        apply_tx_power();   // cap before the first auth/assoc TX burst
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        if (!wifi_ap_mode) {
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

void wifiWaitForConnected(){
    // In AP mode CONNECTED_BIT will never be set; bail after 30s to avoid a hang
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, pdMS_TO_TICKS(30000));
}

void wifiGetIPString(char *out, int len){
    tcpip_adapter_ip_info_t ip;
    // Prefer whichever interface actually has an address: try the joined
    // network (STA) first, then the soft-AP. This is robust even if the
    // wifi_ap_mode flag doesn't match the interface that really got an IP.
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
        return;
    }
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
        return;
    }
    snprintf(out, len, "no IP");
}

// returns true
int isWiFiConnected(){
    EventBits_t bits;
    bits = xEventGroupGetBits( wifi_event_group );
    if(bits & CONNECTED_BIT) 
        return 1;
    return 0;
}


// per-device hostname from CONFIG.JSN settings.hostname (default ctag-modular).
// With several Strämplers on one network, identical names fight over
// <name>.local and mDNS conflict-renaming is flaky — give each unit its own.
static void get_config_hostname(char *out, size_t len)
{
    strlcpy(out, "ctag-modular", len);
    cJSON *root = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
    if (root != NULL) {
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
        cJSON *h = settings ? cJSON_GetObjectItemCaseSensitive(settings, "hostname") : NULL;
        if (h != NULL && cJSON_IsString(h) && h->valuestring[0])
            strlcpy(out, h->valuestring, len);
        cJSON_Delete(root);
    }
}

// live re-apply from the web settings (persisting is the caller's job)
void wifiApplyHostname(const char *name)
{
    if (name && name[0]) {
        mdns_hostname_set(name);
        mdns_instance_name_set(name);
    }
}

static void start_mdns_service()
{
    char host[33];
    get_config_hostname(host, sizeof(host));
    // initialize mDNS
    ESP_ERROR_CHECK(mdns_init());
    // set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK(mdns_hostname_set(host));
    // instance name = hostname so multiple units are tellable apart in
    // discovery lists too
    ESP_ERROR_CHECK(mdns_instance_name_set(host));
    // DHCP hostname too, so router UIs show the right name (next lease)
    tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, host);

    // structure with TXT records
    mdns_txt_item_t serviceTxtData[3] = {
        {"board", "esp32"}, {"u", "user"}, {"p", "password"}};

    // initialize service
    ESP_LOGI("MDNS", "Initialize service...");
    ESP_ERROR_CHECK(mdns_service_add("CTAG-Webserver", "_http", "_tcp", 80,
                                    serviceTxtData, 3));
}

static wifi_config_t buildWifiConfig(){
    cJSON* root = NULL;
    root = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
    wifi_config_t wifi_config;
    if(root != NULL){
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
        if(settings != NULL){
            cJSON *val;
            memset(&wifi_config, 0, sizeof(wifi_config));
            val = cJSON_GetObjectItemCaseSensitive(settings, "ssid");
            strcpy((char*) wifi_config.sta.ssid, val->valuestring);
            val = cJSON_GetObjectItemCaseSensitive(settings, "passwd");
            strcpy((char*) wifi_config.sta.password, val->valuestring);
            val = cJSON_GetObjectItemCaseSensitive(settings, "txpwr");
            if(cJSON_IsNumber(val) && val->valueint >= 8 && val->valueint <= 84)
                s_txpwr = val->valueint;
        }else ESP_LOGE("FILEIO", "settings == NULL");
    }else ESP_LOGE("FILEIO", "root == NULL");
    cJSON_Delete(root);
    return wifi_config;
}

static void start_ap_mode(void)
{
    wifi_ap_mode = 1;
    esp_wifi_disconnect();
    esp_wifi_stop();

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ctag-straempler",
            .ssid_len = 0,
            .channel = 6,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 2,
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power();
    ESP_LOGI(TAG, "AP mode: SSID=ctag-straempler, IP=192.168.4.1");
}

static void ap_sta_retry_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!wifi_ap_mode) continue;
        wifi_sta_list_t stas;
        if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK && stas.num > 0)
            continue;   // someone is on the AP — don't yank it away
        wifi_config_t cfg = buildWifiConfig();
        if (cfg.sta.ssid[0] == 0) continue;
        ESP_LOGI(TAG, "AP idle — retrying STA connection to %s", cfg.sta.ssid);
        restartWifi(&cfg);
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, pdMS_TO_TICKS(15000));
        if (bits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "STA reconnected");
        } else {
            ESP_LOGW(TAG, "STA retry failed — back to AP mode");
            xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
            start_ap_mode();
            xSemaphoreGive(s_wifi_mutex);
        }
    }
}

void initWifi(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    s_wifi_mutex = xSemaphoreCreateMutex();

    wifi_config_t wifi_config = buildWifiConfig();
    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    apply_tx_power();

    // Wait up to 10 seconds for STA connection
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, pdMS_TO_TICKS(10000));
    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        obtain_time();
        start_mdns_service();
    } else {
        ESP_LOGW(TAG, "STA connect timeout — falling back to AP mode");
        start_ap_mode();
        start_mdns_service();
    }
    // reads CONFIG.JSN from SD — leave unpinned (see CLAUDE.md)
    xTaskCreate(ap_sta_retry_task, "sta_retry", 8192, NULL, 5, NULL);
}

void restartWifi(wifi_config_t *cfg){
    if (s_wifi_mutex) xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    esp_wifi_disconnect();   // returns an error in AP mode — harmless
    ESP_ERROR_CHECK( esp_wifi_stop() );
    if (wifi_ap_mode) {
        // leave AP fallback so set_config(STA) doesn't ESP_ERR_WIFI_IF-abort,
        // and re-enable the STA reconnect path in wifi_event_handler
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        wifi_ap_mode = 0;
    }
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, cfg));
    ESP_ERROR_CHECK( esp_wifi_start());
    apply_tx_power();
    if (s_wifi_mutex) xSemaphoreGive(s_wifi_mutex);
}
// ---- ESP-NOW spike shims ---------------------------------------------------
// These exist purely so the LINKER pulls espnow_probe.c in. ld takes only the
// archive members that resolve symbols undefined AT THE MOMENT it scans the
// archive; libwifi.a is scanned before librest-api.a, so nothing had referenced
// espnow_probe_* yet and the member was dropped — an undefined-reference link
// error despite the symbols plainly being in the archive. wifi.c.obj is always
// pulled in (main calls initWifi), so a reference from HERE closes over the
// probe within the same scan.
int  wifiEspnowSetHz(int hz)                                { return espnow_probe_set_hz(hz); }
void wifiEspnowStats(int *hz, unsigned *sent, unsigned *fl) { espnow_probe_stats(hz, sent, fl); }
void wifiEspnowResetStats(void)                             { espnow_probe_reset_stats(); }

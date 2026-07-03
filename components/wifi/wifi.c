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
#include "lwip/apps/sntp.h"
#include "mdns.h"
#include "esp_log.h"
#include "wifi.h"
#include "fileio.h"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;
/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;
static const char *TAG = "WIFI";
static int wifi_ap_mode = 0;

int isWiFiAPMode(void) { return wifi_ap_mode; }

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

#define AP_BIT BIT1

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
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
    case SYSTEM_EVENT_AP_START:
        xEventGroupSetBits(wifi_event_group, AP_BIT);
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

// returns true
int isWiFiConnected(){
    EventBits_t bits;
    bits = xEventGroupGetBits( wifi_event_group );
    if(bits & CONNECTED_BIT) 
        return 1;
    return 0;
}


static void start_mdns_service()
{
    // initialize mDNS
    ESP_ERROR_CHECK(mdns_init());
    // set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK(mdns_hostname_set("ctag-modular"));
    // set default mDNS instance name
    ESP_ERROR_CHECK(mdns_instance_name_set("CTAG Strämpler"));

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
    ESP_LOGI(TAG, "AP mode: SSID=ctag-straempler, IP=192.168.4.1");
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

    wifi_config_t wifi_config = buildWifiConfig();
    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );

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
}

void restartWifi(wifi_config_t *cfg){
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
}
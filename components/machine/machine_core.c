#include <string.h>
#include "machine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MACHINE";
static const machine_t *s_active = NULL;
static void (*s_web_cb)(const machine_t *) = NULL;

void machine_set_web_cb(void (*cb)(const machine_t *m))
{
    s_web_cb = cb;
    if (s_web_cb) s_web_cb(s_active);
}

const machine_t *machine_active(void) { return s_active; }

// see machine.h — knob edits flag here, the menu's autosave poll drains it
static volatile bool s_state_dirty = false;

void machine_state_dirty(void) { s_state_dirty = true; }

bool machine_state_dirty_consume(void)
{
    bool d = s_state_dirty;
    s_state_dirty = false;
    return d;
}

const machine_t *machine_by_name(const char *name)
{
    for (int i = 0; machine_registry[i] != NULL; i++)
        if (strcmp(machine_registry[i]->name, name) == 0)
            return machine_registry[i];
    return NULL;
}

esp_err_t machine_activate(const machine_t *m)
{
    if (m == s_active) return ESP_OK;
    if (s_active) {
        const machine_t *old = s_active;
        ESP_LOGI(TAG, "stopping %s", old->name);
        // the audio task re-reads the active machine every block (~1.45 ms)
        // and outputs silence on NULL; detach first and let the in-flight
        // block drain so stop() never frees memory under a running process()
        s_active = NULL;
        if (s_web_cb) s_web_cb(NULL);   // drop web URIs before stop() frees state
        vTaskDelay(1);   // >=1 tick: pdMS_TO_TICKS(5)==0 at 100Hz = busy-spin
        if (old->stop) old->stop();
    }
    if (m) {
        ESP_LOGI(TAG, "starting %s", m->name);
        esp_err_t err = m->start ? m->start() : ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s failed to start (%d)", m->name, err);
            return err;
        }
        s_active = m;
        if (s_web_cb) s_web_cb(m);
    }
    return ESP_OK;
}

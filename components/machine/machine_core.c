#include <string.h>
#include "machine.h"
#include "esp_log.h"

static const char *TAG = "MACHINE";
static const machine_t *s_active = NULL;

const machine_t *machine_active(void) { return s_active; }

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
    if (s_active && s_active->stop) {
        ESP_LOGI(TAG, "stopping %s", s_active->name);
        s_active->stop();
        s_active = NULL;
    }
    if (m) {
        ESP_LOGI(TAG, "starting %s", m->name);
        esp_err_t err = m->start ? m->start() : ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s failed to start (%d)", m->name, err);
            return err;
        }
        s_active = m;
    }
    return ESP_OK;
}

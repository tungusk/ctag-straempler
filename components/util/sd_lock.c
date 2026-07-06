#include "sd_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_sd_mutex = NULL;

void sd_lock_init(void)
{
    if (s_sd_mutex == NULL)
        s_sd_mutex = xSemaphoreCreateRecursiveMutex();
}

void sd_lock_take(void)
{
    // Lazy safety net: if some early caller runs before mountSDStorage(), create
    // the mutex on demand. Boot is single-threaded up to that point, so the race
    // window is benign.
    if (s_sd_mutex == NULL)
        sd_lock_init();
    xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
}

void sd_lock_give(void)
{
    if (s_sd_mutex != NULL)
        xSemaphoreGiveRecursive(s_sd_mutex);
}

#include "disp_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_disp_mutex = NULL;

void disp_lock_init(void)
{
    if (s_disp_mutex == NULL)
        s_disp_mutex = xSemaphoreCreateRecursiveMutex();
}

void disp_lock_take(void)
{
    // Lazy safety net: if a caller runs before the UI task inits the lock, create
    // it on demand. Boot is single-threaded up to the UI task launch, so the race
    // window is benign.
    if (s_disp_mutex == NULL)
        disp_lock_init();
    xSemaphoreTakeRecursive(s_disp_mutex, portMAX_DELAY);
}

void disp_lock_give(void)
{
    if (s_disp_mutex != NULL)
        xSemaphoreGiveRecursive(s_disp_mutex);
}

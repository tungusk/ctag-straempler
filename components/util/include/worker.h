#pragma once
// worker.h — the lifecycle of a machine's background task (SD reader, module
// renderer, sample player), in one place.
//
// Every machine hand-rolled the same pair of flags, and every copy had the same
// three holes (code review 2026-09-16, plans/code-review-2026-09.md theme B):
//   1. stop() waited a fixed time and then FREED ANYWAY. A reader parked on
//      sd_lock behind a long card write outlives the wait and then copies into
//      freed memory.
//   2. alive was raised by the TASK's first line, so a stop() racing a task that
//      had not been scheduled yet saw "not alive" and freed under it.
//   3. start() set run = true again while a timed-out task was still inside its
//      loop — reviving it next to the new one, two readers on one state.
// Slicer's stop already did (1) right: log and LEAK rather than free. This grows
// that into the shared rule.
//
// Usage:
//   static worker_t s_rd;
//   start():  if (!worker_idle(&s_rd, 2000)) return ESP_ERR_INVALID_STATE;   // FIRST, before any memset
//             ... allocate ...
//             if (!worker_spawn(&s_rd, reader_task, "x_reader", 4096, NULL, 6, NULL)) { free; return ESP_ERR_NO_MEM; }
//   task:     while (s_rd.run) { ... }  cleanup;  worker_exit(&s_rd);
//   stop():   if (!worker_stop(&s_rd, 3000)) { ESP_LOGE(...); return; }   // leak, never free
//             free(...);
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    volatile bool run;      // keep-alive: the task loops while this is set
    volatile bool alive;    // the task exists (raised by spawn, lowered by exit)
} worker_t;

// Spawn the task. alive goes up BEFORE the create, so a stop() that lands before
// the task first runs still waits for it. Refuses (false) while a previous task
// of this worker is still alive. A false return leaves the worker idle.
static inline bool worker_spawn(worker_t *w, TaskFunction_t fn, const char *name,
                                uint32_t stack, void *arg, UBaseType_t prio,
                                TaskHandle_t *out)
{
    if (w->alive) return false;
    w->run = true;
    w->alive = true;
    // unpinned: file-reading tasks pinned to core 0 cause WiFi audio clicks
    if (xTaskCreate(fn, name, stack, arg, prio, out) != pdPASS) {
        w->run = false;
        w->alive = false;
        return false;
    }
    return true;
}

// The task's last line. Never returns.
static inline void worker_exit(worker_t *w)
{
    w->alive = false;
    vTaskDelete(NULL);
}

// Wait up to timeout_ms for a previous task to be gone. start() calls this
// before touching any state the old task might still read.
static inline bool worker_idle(worker_t *w, int timeout_ms)
{
    for (int t = 0; t < timeout_ms && w->alive; t += 10) vTaskDelay(pdMS_TO_TICKS(10));
    return !w->alive;
}

// Ask the task to stop and wait for it. true = it has exited and the caller may
// free what it touches. false = still running: the caller must LEAK its buffers
// (log it), never free them. A leak is a lost slab; a free is a heap corruption.
static inline bool worker_stop(worker_t *w, int timeout_ms)
{
    w->run = false;
    return worker_idle(w, timeout_ms);
}

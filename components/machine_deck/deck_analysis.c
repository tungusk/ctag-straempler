// Deck offline analysis — thin wrapper over the shared engine in
// components/util/bpm_analysis.{h,c} (which is this file's old DSP, lifted so
// DoubleDecker can analyse too). This half keeps the deck-specific policy: the
// backpressure gate (busy = deck playing/loading), the an_* progress/result
// fields, adopting the result through dk.feel, and caching it in the JSN sidecar
// (v2: "dver"/"conf"). The PLL cannot see track-BPM error — p_trk derives from
// the same seg_tf — so the audible lock quality is set entirely by the engine.
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "fileio.h"
#include "sampfile_f.h"
#include "bpm_analysis.h"
#include "deck_priv.h"

static const char *TAG = "DECK-AN";

// the track this run is analysing, snapshotted at start: a new load can race a
// running analysis, and resolving dk.track at commit time stamped the OLD
// track's bpm into the NEW track's sidecar
static char s_an_track[DK_NAME_LEN];

void deck_analysis_commit(void)
{
    if (dk.an_state != DK_AN_DONE || dk.an_bpm <= 0) return;
    if (strcmp(s_an_track, dk.track) == 0) {   // adopt live only if still loaded
        dk.bpm_raw = dk.an_bpm;
        dk.track_bpm = dk.an_bpm * (dk.feel > 0 ? dk.feel : 1.0f);
        dk.grid_offset = dk.an_grid;
    }

    char jp[64];
    sample_resolve_aux(s_an_track, ".JSN", jp, sizeof(jp));
    cJSON *root = readJSONFileAsCJSON(jp);
    if (!root) root = cJSON_CreateObject();
    // sidecar v2: "dver" versions the analysis (missing = v1 -> auto-upgrade on
    // load), "conf" = ACF peak salience. Reserved for a future tempo-map:
    // "anchors": [[frame, beat_index], ...] — readers must ignore it unknown.
    cJSON_DeleteItemFromObjectCaseSensitive(root, "bpm");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "grid");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "dver");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "conf");
    cJSON_AddNumberToObject(root, "bpm", dk.an_bpm);
    cJSON_AddNumberToObject(root, "grid", (double)dk.an_grid);
    cJSON_AddNumberToObject(root, "dver", 2);
    cJSON_AddNumberToObject(root, "conf", (double)dk.an_conf);
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    if (s) { writeJSONFile(jp, s); free(s); }
    ESP_LOGI(TAG, "%s: %.4f BPM (conf %.2f), grid %lu (cached)", s_an_track, dk.an_bpm, dk.an_conf, (unsigned long)dk.an_grid);
}

static bool deck_busy(void) { return dk.playing || dk.loading; }

static void analysis_task(void *pv)
{
    bpm_result_t res;
    int rc = bpm_analyze(s_an_track, deck_busy, &dk.an_progress, &res);
    if (rc != 0) {
        ESP_LOGW(TAG, "analysis failed (%d): %s", rc, s_an_track);
        dk.an_state = DK_AN_FAIL;
        vTaskDelete(NULL);
        return;
    }
    dk.an_bpm = res.bpm;
    dk.an_grid = res.grid;
    dk.an_conf = res.conf;
    dk.an_progress = 100;
    dk.an_state = DK_AN_DONE;
    deck_analysis_commit();          // adopt + cache in the sidecar
    vTaskDelete(NULL);
}

int deck_analyze_start(void)
{
    if (!dk.track[0]) return -1;
    if (dk.an_state == DK_AN_RUNNING) {
        ESP_LOGW(TAG, "analyze_start: already running (%s)", s_an_track);
        return -1;
    }
    strlcpy(s_an_track, dk.track, sizeof(s_an_track));
    dk.an_state = DK_AN_RUNNING;
    dk.an_progress = 0;
    // unpinned (reads files); modest priority so audio + reader stay smooth
    if (xTaskCreate(analysis_task, "deck_an", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "analyze_start: xTaskCreate FAILED (heap %u, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        dk.an_state = DK_AN_FAIL;
        return -1;
    }
    return 0;
}

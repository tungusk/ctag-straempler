#include <string.h>
#include "machine.h"

// Minimal machine: outputs silence. Exists so a build can ship without the
// sampler (the M0d proof) and as a safe fallback for machine switching.

static esp_err_t stub_start(void) { return ESP_OK; }
static void stub_stop(void) {}

static void stub_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK], const machine_io_t *io)
{
    (void)in; (void)io;
    memset(out, 0, MACHINE_BLOCK * sizeof(int32_t));
}

const machine_t machine_stub = {
    .name = "Stub",
    .start = stub_start,
    .stop = stub_stop,
    .process = stub_process,
};

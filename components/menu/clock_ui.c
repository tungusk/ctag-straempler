// clock_ui — write-through Setup-row helpers for the core clock. See clock_ui.h.
#include "clock_ui.h"
#include "machine.h"
#include "menu_config.h"

static bool s_dirty;

static void mark(void) { s_dirty = true; machine_state_dirty(); }

int clock_ui_cycle_src(int dir)
{
    int s = clock_source_cycle_cv_audio(clock_core_src(), dir);
    clock_core_set_src(s);
    mark();
    return s;
}

void clock_ui_set_src(int src)
{
    clock_core_set_src(src);
    mark();
}

float clock_ui_cycle_ppq(int dir)
{
    static const float ladder[4] = { 1, 2, 4, 8 };
    float cur = clock_core_ppb();
    int k = 2;
    for (int i = 0; i < 4; i++) if (cur >= ladder[i] - 0.1f && cur <= ladder[i] + 0.1f) k = i;
    k = (k + (dir > 0 ? 1 : 3)) % 4;
    clock_core_set_ppb(ladder[k]);
    mark();
    return ladder[k];
}

float clock_ui_adj_bpm(float d)
{
    float b = clock_core_int_bpm() + d;
    clock_core_set_int_bpm(b);
    mark();
    return clock_core_int_bpm();
}

bool clock_ui_toggle_auto(void)
{
    bool on = !clock_core_auto();
    clock_core_set_auto(on);
    mark();
    return on;
}

void clock_ui_flush(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    configSetIntSetting("clk_src",  clock_core_src());
    configSetIntSetting("clk_ppq",  (int)(clock_core_ppb() + 0.5f));
    configSetIntSetting("clk_bpm",  (int)(clock_core_int_bpm() + 0.5f));
    configSetIntSetting("clk_auto", clock_core_auto() ? 1 : 0);
}

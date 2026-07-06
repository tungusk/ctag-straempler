#pragma once
#include <stdint.h>
#include <stdbool.h>

// M3 slicer — ONE stereo sample loaded into PSRAM, chopped into an equal grid
// of slices; one monophonic (retrigger) voice plays a slice on trigger. All
// controls act on this single track. Engine (audio task) owns playback; the
// UI task writes settings + one-shot command flags and reads for display.

#define SL_RATE       44100
#define SL_MAX_SECS   12
#define SL_MAX_FRAMES (SL_RATE * SL_MAX_SECS)   // stereo frames cap
#define SL_PEAKS      300                       // waveform display columns
#define SL_MAX_SLICES 32

typedef struct {
    int16_t *buf;                 // PSRAM interleaved stereo L,R, SL_MAX_FRAMES*2
    volatile uint32_t len;        // frames loaded
    volatile bool loading;        // true while (re)loading — engine stays silent
    char sample[24];              // loaded sample id (no extension)

    // settings (UI writes, engine reads)
    volatile int  slice_target;   // requested slice count 8/16/32 (grid = exact,
                                  // transient = max)
    volatile int  n_slices;       // ACTUAL slice count in use
    volatile bool transient_mode; // slice at detected transients vs equal grid
    volatile int  sensitivity;    // 0..100 transient threshold (higher = more)
    uint32_t slice_pt[SL_MAX_SLICES + 1];  // slice boundaries (frames), n_slices+1
    volatile int  sel;            // selected slice (UI/CV target)
    volatile bool auto_on;        // auto-advance through slices on slice end
    volatile bool reverse;        // play slices reversed
    volatile uint16_t level;      // 0..255 output level (CV6)
    volatile uint16_t pitch_cv;   // 0..4095 pitch control (CV7)

    // playback voice (engine)
    volatile bool playing;
    volatile int  cur;            // slice currently playing (for display)
    double  pos;                  // fractional frame position
    uint32_t s_start, s_end;      // current slice bounds (frames)
    float   inc;                  // pitch increment (frames per output sample)

    // one-shot commands from UI (engine consumes)
    volatile uint8_t cmd_fire;    // fire sel slice
    volatile uint8_t cmd_advance; // fire sel, then step sel to next

    // waveform display (computed on load)
    volatile int  peak_n;
    uint8_t peaks[SL_PEAKS];      // 0..31 mono peak height per column
} sl_state_t;

extern sl_state_t sl;

// load a stereo RAW from /sdcard/usr/<name>.RAW into PSRAM (UI task). 0 = ok.
int slicer_load(const char *name);
// recompute slice boundaries from slice_target + mode (UI task; mutes briefly)
void slicer_reslice(void);
// list usr/*.RAW ids into out[][], returns count (<= max)
int slicer_list_samples(char out[][24], int max);

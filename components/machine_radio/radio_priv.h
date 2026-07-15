#pragma once
#include <stdint.h>
#include <stdbool.h>

// Internet radio machine — streams an icecast/shoutcast MP3 station: an HTTP
// reader task pulls the endless body, helix decodes it frame-by-frame, PCM lands
// in a PSRAM ring, and process() drains the ring (deck discipline: process()
// only ever reads the ring). v1 accepts 44.1 kHz mono/stereo MP3 and rejects
// other rates (the cubic resampler is a v2 add). No SD in the path.

#define RADIO_RATE        44100
#define RADIO_RING_FRAMES (RADIO_RATE * 4)     // 4 s stereo ring (~690 KB PSRAM)
#define RADIO_LOW_WATER   (RADIO_RATE / 2)     // pre-buffer ~0.5 s before playing
#define RADIO_IN_SIZE     8192                 // socket->decoder byte buffer (internal)
#define RADIO_MIN_FRAME   1600                 // decode only with > a max MP3 frame present
#define RADIO_URL_LEN     176
#define RADIO_NAME_LEN    24
#define RADIO_TITLE_LEN   80          // ICY now-playing "StreamTitle"

enum { RADIO_STOPPED = 0, RADIO_BUFFERING, RADIO_PLAYING, RADIO_ERROR };

#define RADIO_MAX_ST      24         // built-in defaults + SD-saved favorites
typedef struct { char name[RADIO_NAME_LEN]; char url[RADIO_URL_LEN]; } radio_station_t;

typedef struct {
    int16_t *ring;                 // PSRAM interleaved stereo
    volatile uint32_t wpos, rpos;  // monotonic frame counters (SPSC)
    volatile int  state;           // RADIO_*
    volatile int  bitrate;         // kbps (0 = unknown yet)
    volatile int  samprate;        // native rate reported by the stream
    volatile int  nchans;
    volatile uint32_t underruns;
    char url[RADIO_URL_LEN];       // active stream URL
    char station[RADIO_NAME_LEN];  // active display name
    char title[RADIO_TITLE_LEN];   // ICY now-playing (empty if none)
    char err[56];                  // last error (shown in state + UI)
    volatile uint32_t reconnects;  // auto-reconnects this session
    int  sel;                      // on-device station selector position
} radio_state_t;

extern radio_state_t rd;
extern radio_station_t rd_stations[];   // [0..RADIO_N_DEFAULT) built-in, then SD-saved
extern int rd_n_stations;
#define RADIO_N_DEFAULT 5               // the first N are built-in (not deletable)

// control — safe from the httpd task (web) and the UI task (menu)
void radio_play_url(const char *url, const char *name);
void radio_play_station(int idx);
void radio_stop_stream(void);

// station favorites (SD-persisted in usr/radio.jsn)
void radio_stations_load(void);
int  radio_station_add(const char *name, const char *url);   // 0 ok, <0 full
int  radio_station_del(int idx);                             // saved-only; 0 ok

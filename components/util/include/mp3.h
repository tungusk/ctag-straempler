#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void initMP3Engine(xQueueHandle);
void decodeMP3File(const char *id);

// synchronous decode in the caller's task (no UI events): fin/fout are FatFS
// paths (no /sdcard prefix). out_channels/out_samprate (optional) receive the
// mp3's channel count and sample rate — the decoder does NOT resample, so a
// caller installing into the 44.1 kHz library must check out_samprate.
// progress_cb (optional) is called with 0..100. Returns 0 on success, -1 on
// open/decode failure. One decode at a time (shared buffer).
int decodeMP3FileSync(const char *fin, const char *fout,
                      int *out_channels, int *out_samprate,
                      void (*progress_cb)(int pct, void *arg), void *arg);

//void decodeMP3(unsigned char* inputData, unsigned int len, FILE* outfile);
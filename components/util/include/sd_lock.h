#pragma once

// Global SD-card access lock.
//
// The audio playback path uses raw FatFS calls (f_read/f_lseek) that BYPASS the
// esp_vfs_fat per-volume lock, so they can race the VFS calls made by REST file
// serving, recording, and config I/O — all on the one SD/SPI bus. That race can
// wedge the bus and corrupt the FAT/directory (empty file browser, noisy reads).
//
// Every subsystem that touches the SD card must hold this lock around its actual
// I/O burst (one f_read/fwrite/chunk), releasing between bursts so the real-time
// audio reader tasks (triple-buffered) are never starved. It is a RECURSIVE
// mutex so nested SD helpers on the same task don't self-deadlock.
//
// Lock ordering rule: always acquire a per-voice file_mutex BEFORE sd_lock
// (never the reverse), so the two lock families can't deadlock.

void sd_lock_init(void);   // create the mutex; call once at SD mount, pre-tasks
void sd_lock_take(void);   // block until the SD bus is ours
void sd_lock_give(void);   // release the SD bus

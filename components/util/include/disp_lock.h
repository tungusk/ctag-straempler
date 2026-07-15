#pragma once

// Global TFT/display access lock.
//
// The ILI9341 is a single SPI device driven from ONE place — the UI event task
// (ui_ev_loop): every draw funnels through menuProcessEvent there, and the
// repeating timers only POST events into that queue, so on-device drawing is
// naturally single-threaded. The /screenshot REST handler breaks that
// invariant: it reads the panel's GRAM back (TFT_RAMRD) from the httpd task,
// which would interleave SPI transactions with a UI draw and corrupt both the
// readback and the display.
//
// So every party that touches the TFT/SPI bus holds this lock around its burst:
// the UI task around each event it processes (one menuProcessEvent), and the
// screenshot reader around each row it reads back. Held only for the burst, so
// neither side starves the other — the screenshot releases between rows so a
// long network stall can't freeze the UI, and the UI releases between events so
// a capture waits at most one event.
//
// Lock ordering: the UI path may take disp_lock and THEN sd_lock (a load draws
// then hits the card); nothing takes them in the reverse order, and the
// screenshot path takes disp_lock only — so the two families can't deadlock.
// Recursive so nested menusys draw helpers on the UI task don't self-deadlock.

void disp_lock_init(void);   // create the mutex; call once before the UI task
void disp_lock_take(void);   // block until the display bus is ours
void disp_lock_give(void);   // release the display bus

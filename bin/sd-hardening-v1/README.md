# sd-hardening-v1

Core (all-machine) robustness pass for SD-card access. Built on the
seven-machine `v09-machines` line; boots into the persisted machine (Sampler2
at capture time). Flash with `./flash.sh [PORT]` (default port
`/dev/cu.usbserial-3110`, `--flash_size detect`).

## What changed vs granular/glitch/looper-v2 era

Two independent SD bugs, both fixed:

1. **Audio noise / bus wedge — SD access contention.**
   The audio playback path uses *raw* FatFS `f_read`/`f_lseek` that bypass the
   esp_vfs_fat per-volume lock, so they could race the VFS I/O from REST file
   serving, recording, and config — all on the one SD/SPI bus. That race could
   wedge the bus (empty/garbled reads) and risked FAT corruption.
   **Fix:** a global recursive `sd_lock` (`components/util/sd_lock.{h,c}`, init
   at mount) held around every SD I/O burst by *all* subsystems — raw audio
   reads, `fileio` JSON, recording writer, all REST handlers, `sample_ram`.
   Acquired *inside* the per-voice `file_mutex` (consistent order, no deadlock),
   released between bursts so the triple-buffered audio readers never starve.

2. **Empty web file browser — internal-RAM starvation + PSRAM/DMA limit.**
   `readJSONFileAsCJSON` read sidecar/config JSON into a **PSRAM** buffer, but
   SDMMC DMA can't target PSRAM — it needs an on-the-fly internal bounce buffer,
   which fails `ESP_ERR_NO_MEM (257)` when internal RAM is tight (WiFi + 6×8 KB
   audio buffers + tasks). Every sidecar read failed → list came back empty,
   and building the whole ~86-entry cJSON array + printing it also OOM'd.
   **Fix:** (a) JSON reads now use internal DMA-capable RAM (`MALLOC_CAP_DMA`);
   (b) `GET /files` **streams** one small object per file as it walks the dir
   and **skips per-file sidecar reads** (names from `readdir`, sizes from
   `stat`) — flat memory, no OOM, no slow failing reads / socket resets.
   Tradeoff: descriptions/tags are blank in the listing for now (a later fix
   should relieve internal-RAM pressure, e.g. route cJSON to PSRAM).

The SD card was verified healthy on a PC (fsck) — there was never on-disk
corruption; symptoms were runtime memory/contention only.

## Web UI + status additions (same build)

- **CV meters** fixed (were upside-down; now bottom-anchored flex fill).
- **Device IP** shown on the web Settings tab AND on the module's
  **System → Settings** screen (new read-only 5th row). `wifiGetIPString()`
  now checks the STA interface first, then AP, so it works regardless of the
  `wifi_ap_mode` flag.
- **SD free/total** in the web footer (`SD n/mMB`) via new `GET /sysinfo`
  (`f_getfree`, fetched on-demand — never in the hot /status poll).
- **Download button** (⬇) per file row — saves `NAME.RAW` via `/files/raw`.
- **Removed**: the Play button (worked but fatally slow — whole 2 MB file must
  cross WiFi before any sound; also monopolised the server) and the
  Description/Tags columns (matched the streamed, sidecar-free /files).

## Known follow-ups (not in this build)
- Editable descriptions/tags in the browser (needs the internal-RAM fix to
  read sidecars again — e.g. route cJSON to PSRAM).
- Optional: progressive/stop-able in-browser preview, or trigger playback on
  the module instead of the browser.
- Optionally extend `sd_lock` to the frozen original `Sampler` machine.

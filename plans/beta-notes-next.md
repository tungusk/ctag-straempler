# Release-note musts for the next beta (after `v0.10-beta2`)

Running draft. Same voice as the beta2 notes: what changed, what to read
before flashing, a numbered test checklist. Fill the stubs as the work lands.

## Faster display (`settings.tftclk`) — DRAFTED 2026-09-10

> **The screen is quicker in this build.** The display's SPI write clock moved
> from 26 MHz to 40, which takes about 27% off a full page repaint — a Synth
> machine switch goes 409 → 297 ms, Tracker 354 → 248 ms. Nothing else changed
> about drawing; it is the same pixels, sent faster. It costs the audio side
> nothing measurable (we checked with a held note under continuous repaints).
>
> 40 MHz is the usual overclock for this panel and was bit-exact over 25 runs
> of a bit-stress pattern test on our units. If yours turns out not to like it
> — speckled pixels, torn rows, or a screen full of garbage after the update —
> put it back:
>
> ```
> curl -X POST http://<ip>/settings -d '{"tftclk":26}'
> ```
>
> A wrong clock only affects drawing: the module keeps running and the web
> remote keeps answering, so you can always fix it from the browser. If you
> have not set up WiFi yet, add `"tftclk": 26` to the `settings` object in
> `CONFIG.JSN` on the card instead. **Please tell us if you have to do this** —
> we have only tested on our own boards, and yours is the interesting data
> point. Only 26, 40 and 80 are real clock dividers; anything else rounds.
>
> If you have bridged **SJ1** (the display MISO solder jumper, open from the
> factory) you can test your panel directly:
>
> ```
> curl "http://<ip>/tftread?pattern=1&clk=40"
> ```
>
> writes 1280 pixels of bit-stress patterns at 40 MHz and reads them back at
> 1 MHz. Zero mismatches means your panel takes the clock. Add `&keep=1` to
> leave the stripe on screen for an eyeball.

Checklist item to add: *"Switch machines a few times and watch the screen — it
should be visibly quicker than beta2, with no speckle, torn rows or colour
noise. If it is dirty, `POST /settings {"tftclk":26}` and tell us."*

Configuration section: add `settings.tftclk` next to `encres` / `encdir` /
the `clk_*` keys, noting the default is now 40.

## Remote tab: FX card — STUB
One control group per effect kind in a slot; FX1/FX2 pickers, FX3 reverb + mix;
CV MATRIX card has its own Apply.

## Remote tab: machine Setup mirror (`/remote/setup`) — STUB
The machine's own Setup page, rendered from its `setup_menu_t` with its own
callbacks; edits queue `EV_REMOTE_SETUP` and land as if turned on the panel.

## Remote tab: settings cards — STUB
MACHINE SETUP / GLOBAL as TFT-look screens, ADVANCED as the raw-key table;
all three fold, closed on first launch, remembered per browser.

## Also worth a line
- `/sysinfo` now carries a `tft` object (clock, shadow FB, per-event and
  per-tick redraw timings) and `?tftclear=1` resets it.
- `/screenshot` is RGB565 now (smaller, and it no longer disturbs Tape).

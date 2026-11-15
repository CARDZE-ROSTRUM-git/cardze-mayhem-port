# Changes in this build

Summary of what changed in the desktop port. Upstream firmware files are never
touched; only the host layer under `platform/` is modified.

## Setup window (pre-boot)

* **Input mode selector** — choose `Touchscreen + buttons` (default, keeps the
  on-screen PortaPack H2 button deck) or `Touchscreen only` (hides the deck so
  the window is just the screen). The top strip (rotate / FS / DFU) and the
  silkscreen band with the owner handle stay in both modes.
* **Window rotation selector** — same effect as the `R` key, but the chosen
  value is **saved as the default** and applied on the next launch.
* Saved setting lives in `$XDG_CONFIG_HOME/mayhem-desktop/settings.conf`
  (key `rotation=NNN`).

## Remote app fix

* Opening **Remote** (and the other transmit images) used to show a dead black
  band: those images never run the receive chain, so no spectrum ever reached
  the app's `WaterfallView`. The host now feeds a dim, slightly noisy floor at
  the waterfall frame rate (~50 Hz) while streaming is on and no real spectrum
  is produced, so the band scrolls and the app looks alive. RX is unaffected —
  the fallback only runs when the receive chain produces nothing.

## Files changed (6)

| File | Change |
|------|--------|
| `platform/include/lcd_sdl.hpp` | declare `set_deck_visible` / `deck_visible`, `set_rotation` / `rotation` |
| `platform/src/lcd_sdl.cpp`   | `g_deck_visible` + `dev_h()`, hide the deck, rotate + resize the window |
| `platform/include/host_launcher.hpp` | `Result.touch_only`, `Result.rotation`, `saved_rotation()` |
| `platform/src/host_launcher.cpp`     | input-mode row, rotation row, settings load/save |
| `platform/src/host_baseband.cpp`     | `feed_floor_spectrum()` / `feed_floor_if_due()` for the Remote fix |
| `platform/src/main.cpp`              | apply the chosen input mode and rotation |

## Binary

`mayhem-desktop` — x86-64 Linux executable built from this tree
(`cmake -DCMAKE_BUILD_TYPE=Release`). Built and linked against SDL2, optional
RTL-SDR (RX only) and PlutoSDR (RX + TX, unfinished). Not stripped.

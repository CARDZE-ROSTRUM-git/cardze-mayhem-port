# Changes in this build

Summary of what changed in the desktop port. Most of the work lives in the host
layer under `platform/`; the shipped binary also carries one menu edit in the
pinned firmware checkout (the HackRF tile was replaced with Snake).

## Latest build

* **Battery capacity now makes sense.** The battery screen shows the pack's
  rated capacity instead of the worn-down full-charge estimate. On Linux it is
  read from the sysfs design fields; on the device it comes from the MAX17055.
* **Capacity can be entered by hand.** The pre-boot menu has a Battery row
  beside the SDR choices. Type the mAh value, or use the on-screen `-` / `+`
  buttons (10 mAh per tap, accelerating while held). An empty field or `0`
  keeps auto-detection; the chosen value is saved and applied at startup.
* **HackRF mode is gone from the home menu.** On a desktop it could stop the
  firmware UI thread and leave the window stuck. Its tile now opens Snake.
* **On-screen cross keys were redrawn.** They stay square and sit at equal
  distances from OK whatever shape the window is resized to, with matching
  edges on every side.
* **More RX apps are usable.** Six additional receive backends were wired into
  the host M4 layer, bringing the working set to twelve.

## Setup window (pre-boot)

* **Input mode selector** — choose `Touchscreen + buttons` (default, keeps the
  on-screen PortaPack H2 button deck) or `Touchscreen only` (hides the deck so
  the window is just the screen). The top strip (rotate / FS / DFU) and the
  silkscreen band with the owner handle stay in both modes.
* **Window rotation selector** — same effect as the `R` key, but the chosen
  value is **saved as the default** and applied on the next launch.
* Saved settings live in `$XDG_CONFIG_HOME/mayhem-desktop/settings.conf`
  (`rotation=NNN`, `touch_only=0|1`, `battery_mah=NNN`).

## Remote app fix

* Opening **Remote** (and the other transmit images) used to show a dead black
  band: those images never run the receive chain, so no spectrum ever reached
  the app's `WaterfallView`. The host now feeds a dim, slightly noisy floor at
  the waterfall frame rate (~50 Hz) while streaming is on and no real spectrum
  is produced, so the band scrolls and the app looks alive. RX is unaffected —
  the fallback only runs when the receive chain produces nothing.

## Files changed in the host layer

| File | Change |
|------|--------|
| `platform/include/lcd_sdl.hpp`, `platform/src/lcd_sdl.cpp` | Launcher release events, square/centered D-pad, deck visibility, rotation |
| `platform/include/host_launcher.hpp`, `platform/src/host_launcher.cpp` | Battery row and mAh dialog, input-mode and rotation rows, settings |
| `platform/include/host_settings.hpp`, `platform/src/host_settings.cpp` | Saved `battery_mah`, rotation, input mode and key bindings |
| `platform/src/host_battery.cpp` | Rated capacity from sysfs design fields, launcher override |
| `platform/src/main.cpp` | Applies saved rotation, input mode and battery capacity |
| `platform/include/host_m4.hpp`, `platform/src/host_m4.cpp`, `cmake/M4Decoders.cmake` | Additional RX backends on the host M4 side |
| `platform/src/host_baseband.cpp`, `platform/src/sdr_rtlsdr.cpp` | Waterfall floor fallback and gain fixes |

## Binary

`mayhem-desktop` — x86-64 Linux executable built from this tree
(`cmake -DCMAKE_BUILD_TYPE=Release`). Built and linked against SDL2, optional
RTL-SDR (RX only) and PlutoSDR (RX + TX, unfinished). Not stripped.

Support / feedback: `@cardputer_zero`.

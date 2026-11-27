# cardze-mayhem-port

**English** | [Русский](README.ru.md)

> **BETA** — early, work-in-progress port. Expect rough edges and unfinished
> features.

A desktop port of the **PortaPack Mayhem** firmware (HackRF One + PortaPack H2)
that runs natively on Linux as an SDL2 application.

## Tested on

* Tested **only** under the **niri** window manager (Wayland). Other desktops and
  compositors have not been exercised.
* Designed for and tested on **cyberdecks** with a **7-inch 1024x600** display.

The original Mayhem UI is used **unmodified**: menus, widgets, fonts, icons and
the 240x320 layout are the real firmware code. Only the low layers are replaced:

| Firmware (target)          | This port (host)                                  |
|----------------------------|---------------------------------------------------|
| ChibiOS threads/events     | `std::thread` / mutex / condition variable        |
| LPC43xx M4 + M0 cores      | host threads                                      |
| ILI9341 LCD over SPI       | RGB565 framebuffer + SDL2 textures                |
| Physical buttons/encoder   | keyboard + on-screen PortaPack H2 body            |
| FatFs SD card              | a plain directory (`mayhem-sd/`)                   |
| RTL-SDR / PlutoSDR         | libiio (`libiio`) and `librtlsdr` backends        |

All **102 upstream external apps** (RX / TX / Games / Utilities / Settings /
Debug tabs) are compiled into the host binary.

## ⚠️ SDR support — read this first

**Only two SDR backends are supported:**

* **PlutoSDR** (via `libiio`) — full RX **and TX**
* **RTL-SDR** (via `librtlsdr`) — **RX only**

There is no HackRF / PortaPack USB support: this is a *port*, not an emulator,
so the original HackRF hardware is not used.

## ⚠️ TX is unfinished (ALPHA)

TX support is **experimental and unverified on air**. The main developer **does
not own a PlutoSDR yet**, so TX has only been exercised against the SDR backends
in code — **not confirmed on real hardware**. Treat every TX feature as
untested. RX is the tested path.

## Features

* Real Mayhem firmware UI, scaled x1 / x2 / x3, framed as a PortaPack H2 body.
* Keyboard as an alternative to the on-screen buttons; mouse = touchscreen.
* Pre-boot **setup window**: pick the SD folder, the SDR device, the input mode
  (touchscreen + on-screen buttons, or **touchscreen only**) and the window
  rotation.
* The rotation picked in the setup window is **saved** and reused on the next
  launch.
* File-based TX stub when no transmit-capable SDR is present.
* Floating window by default; `F` / `F11` toggles borderless fullscreen
  (body keeps its aspect ratio, letterboxed and centered).
* Emulated SD card is a folder, so settings, captures and playlists travel with
  the app.

## Install from scratch (Arch / EndeavourOS)

```sh
# 1. Build dependencies + SDR libraries
sudo pacman -S --needed base-devel cmake git pkgconf sdl2 sdl2_image \
    rtl-sdr libiio libad9361-iio python

# 2. Get this port
git clone https://github.com/CARDZE-ROSTRUM-git/cardze-mayhem-port.git
cd cardze-mayhem-port

# 3. Get the upstream firmware at the pinned commit
git clone https://github.com/portapack-mayhem/mayhem-firmware.git \
    third_party/mayhem-firmware
git -C third_party/mayhem-firmware checkout 77798cb09f227de5c58dea48899ddb7448b44a07

# 4. Build
mkdir -p build && cd build
cmake ..
cmake --build . -j"$(nproc)"

# 5. Run
cd .. && cp -f build/mayhem-desktop . && ./run.sh
```

### Debian / Ubuntu equivalent

```sh
sudo apt install build-essential cmake git pkg-config libsdl2-dev sdl2-image \
    librtlsdr-dev libiio-dev libad9361-dev python3
```

Then repeat steps 2–5 above.

## Packages / requirements

| Package        | Needed for                     |
|----------------|--------------------------------|
| `cmake`, `git`, `pkgconf` | build system          |
| `base-devel` / `build-essential` | C/C++ toolchain  |
| `sdl2`         | window, input, audio           |
| `sdl2_image`   | image loading (icons/screens)  |
| `rtl-sdr`      | RTL-SDR backend (RX only)      |
| `libiio`       | PlutoSDR backend (RX + TX)     |
| `libad9361-iio`| PlutoSDR RF helpers            |
| `python`       | `tools/gen_external.py`        |

The RTL-SDR / PlutoSDR backends are compiled **only when their library is
found**. Without them the app still builds and runs with the file TX stub.

## Run

`run.sh` sits next to the binary and its emulated SD card:

```sh
./run.sh
```

It sets `MAYHEM_SD` to the sibling `mayhem-sd/` folder and picks a sensible SDL
video driver. Override the SD location if you want:

```sh
MAYHEM_SD=~/my-mayhem-sd ./run.sh
```

### Keyboard controls

| Key(s)                | Action                          |
|-----------------------|---------------------------------|
| Arrow keys            | D-pad (Up/Down/Left/Right)      |
| Enter / Space         | Select                          |
| Esc                   | Back                            |
| Mouse wheel / `<` `>` | Encoder                         |
| `1` / `2` / `3`       | Zoom x1 / x2 / x3               |
| `R`                   | Rotate the body                 |
| `F` / `F11`           | Toggle fullscreen               |

## Project layout

```
platform/include/   Host shims: ChibiOS/HAL/NVIC, LCD, input, SDR, launcher
platform/src/       Host implementations of the replaced firmware layers
cmake/              Generated upstream + external-app source lists
tools/              gen_external.py (regenerates the external app tables)
CMakeLists.txt      Host build (SDL2, optional RTL-SDR / PlutoSDR)
run.sh              Launcher for the built binary + emulated SD card
HANDOFF.md          Development notes (Russian)
```

## Roadmap

* **Android support** — planned.
* **Windows support** — planned.

## Known issues

* In **non-system apps** the waterfall renders poorly — it stutters and updates
  unevenly compared to the built-in system apps.

## Notes

* Upstream files are **never modified**; GPL headers are preserved verbatim.
  New host files are marked as part of this GPL derivative work.
* Not every baseband statistics message is wired up yet — see `HANDOFF.md`.

## License

Derivative work of the GPL-licensed PortaPack Mayhem firmware (see upstream
`LICENSE` and `LICENSE.GPL-2.0-or-later`). Distributed under the same terms; all
upstream copyright and license notices are retained.

## Author / contact

Maintained by **[@CARDPUTER_ZERO](https://t.me/cardputer_zero)** on Telegram.

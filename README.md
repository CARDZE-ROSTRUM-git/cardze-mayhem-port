# cardze-mayhem-port

A desktop port of the **PortaPack Mayhem** firmware (HackRF One + PortaPack H2)
that runs natively on Linux as an SDL2 application.

The original Mayhem UI is used **unmodified**: menus, widgets, fonts, icons and
the 240x320 layout are the real firmware code. Only the low layers are replaced:

| Firmware (target)          | This port (host)                                  |
|----------------------------|---------------------------------------------------|
| ChibiOS threads/events     | `std::thread` / mutex / condition variable        |
| LPC43xx M4 + M0 cores      | host threads                                      |
| ILI9341 LCD over SPI       | RGB565 framebuffer + SDL2 textures                |
| Physical buttons/encoder   | keyboard + on-screen PortaPack H2 body            |
| FatFs SD card              | a plain directory (`mayhem-sd/`)                   |
| Baseband samples           | host SDR backend (RTL-SDR / PlutoSDR) + file stub  |

All **102 upstream external apps** (RX / TX / Games / Utilities / Settings /
Debug tabs) are compiled straight into the host binary, so the full Mayhem app
list works on the desktop.

## Features

* Real Mayhem firmware UI, scaled x1 / x2 / x3, framed as a PortaPack H2 body.
* Keyboard works as an alternative to the on-screen buttons; mouse = touchscreen.
* Optional SDR backends: RTL-SDR and PlutoSDR, selected at build time.
* File-based TX replay when no transmit-capable SDR is present.
* Floating window by default, with `F` / `F11` toggle to borderless fullscreen
  (body keeps its aspect ratio, letterboxed and centered).
* Emulated SD card is a folder, so settings, captures and playlists travel with
  the app.

## Requirements

* Linux, C++20 compiler (GCC or Clang), CMake >= 3.16
* SDL2 (`libsdl2-dev`)
* Optional: `librtlsdr` for RTL-SDR, `libiio` for PlutoSDR
* The upstream Mayhem firmware source tree (see below)

### Install build dependencies (examples)

```sh
# Arch / EndeavourOS
sudo pacman -S base-devel cmake sdl2 rtl-sdr libiio

# Debian / Ubuntu
sudo apt install build-essential cmake libsdl2-dev librtlsdr-dev libiio-dev
```

## Get the upstream firmware

This repository contains **only the port's own code** (the `platform/` layer,
`cmake/` helpers and tooling). The upstream firmware is **not** vendored here.
Clone it into `third_party/mayhem-firmware` at the pinned commit:

```sh
git clone https://github.com/portapack-mayhem/mayhem-firmware.git \
    third_party/mayhem-firmware
git -C third_party/mayhem-firmware checkout 77798cb09f227de5c58dea48899ddb7448b44a07
```

The port is developed against upstream commit `77798cb0`
(2026-11-07). Upstream is never patched; all host adaptation lives in
`platform/` and `cmake/`, applied through forced-includes and include shadowing.

## Build

```sh
mkdir -p build && cd build
cmake ..
cmake --build . -j"$(nproc)"
```

The result is the `mayhem-desktop` binary. Copy it next to `run.sh` and the
`mayhem-sd/` folder (or just run it from the build directory with `MAYHEM_SD`
pointing at an SD folder).

The RTL-SDR / PlutoSDR backends are compiled **only when the matching library is
found**; without them the app still builds and runs with the file TX stub.

## Run

`run.sh` is a small launcher that sits next to the binary and its emulated SD
card:

```sh
./run.sh
```

It sets `MAYHEM_SD` to the sibling `mayhem-sd/` folder and picks a sensible SDL
video driver for the current session. You can override the SD location:

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
HANDOFF.md          Detailed development notes (Russian)
```

## Status / notes

* Upstream files are **never modified**; the GPL headers are preserved verbatim.
  New host files are marked as part of this GPL derivative work.
* `HANDOFF.md` (in Russian) documents the porting details, known traps and what
  has been verified live on real hardware (RTL-SDR v2 / no TX-capable device
  available for on-air tests).
* Not every statistics message from the baseband side is wired up yet; see
  `HANDOFF.md` for the current gaps.

## License

This port is a derivative work of the GPL-licensed PortaPack Mayhem firmware
(see upstream `LICENSE` and `LICENSE.GPL-2.0-or-later`). It is distributed under
the same terms. All upstream copyright and license notices are retained.

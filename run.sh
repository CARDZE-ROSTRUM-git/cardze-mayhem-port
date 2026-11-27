#!/bin/sh
# Launcher for the PortaPack Mayhem desktop port.
#
# This folder is self-contained: the binary sits next to its own emulated SD
# card ("mayhem-sd/"), so copying the whole "Mayhem Desktop" folder anywhere
# keeps the settings, captures and playlists with it.
#
# The firmware's SD card is emulated as a directory selected by the MAYHEM_SD
# environment variable; point it at the sibling folder regardless of the path
# this script is invoked through.
set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$HERE/mayhem-desktop"

if [ ! -x "$APP" ]; then
    echo "mayhem-desktop: binary not found next to this launcher: $APP" >&2
    exit 1
fi

# Keep an operator-provided override, otherwise use the sibling SD folder.
: "${MAYHEM_SD:=$HERE/mayhem-sd}"
export MAYHEM_SD
mkdir -p "$MAYHEM_SD"

# Prefer the native Wayland driver when the session offers one: SDL2 otherwise
# picks x11 (XWayland) whenever DISPLAY is also set, and under niri the XWayland
# window renders empty/garbled. Fall back to x11 only when there is no Wayland
# display, and always keep an explicit operator override.
if [ -z "${SDL_VIDEODRIVER:-}" ]; then
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then
        SDL_VIDEODRIVER=wayland
    elif [ -n "${DISPLAY:-}" ]; then
        SDL_VIDEODRIVER=x11
    fi
    [ -n "${SDL_VIDEODRIVER:-}" ] && export SDL_VIDEODRIVER
fi

exec "$APP" "$@"

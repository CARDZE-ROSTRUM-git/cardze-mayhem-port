cardze-mayhem-port — desktop port of the PortaPack Mayhem firmware
==================================================================

This container image exists to publish the prebuilt Linux binary through GitHub
Packages (GitHub Packages cannot host a bare file, only package formats).

The executable is at /mayhem-desktop inside the image. Extract it with either of:

  # 1. copy it out of a container
  id=$(docker create ghcr.io/cardze-rostrum-git/cardze-mayhem-port:latest)
  docker cp "$id":/mayhem-desktop ./mayhem-desktop
  docker rm "$id"
  chmod +x ./mayhem-desktop

  # 2. export the whole filesystem
  docker create --name mdp ghcr.io/cardze-rostrum-git/cardze-mayhem-port:latest
  docker export mdp | tar -x ./mayhem-desktop
  docker rm mdp

The binary needs a Linux host with SDL2 (and librtlsdr / libiio for SDR) plus an
X11 or Wayland session. For normal use prefer the release asset or a local build
— see README.md / README.ru.md.

Maintained by @CARDPUTER_ZERO (https://t.me/cardputer_zero).

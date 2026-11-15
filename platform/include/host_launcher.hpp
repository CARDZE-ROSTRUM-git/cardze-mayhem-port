/*
 * Pre-boot setup window (launcher) for the desktop PortaPack.
 *
 * On the device there is nothing to configure before the firmware runs: the SD
 * card is wherever it was inserted and the radio is the one soldered to the
 * board. The desktop port has neither, so this window collects the two choices
 * the firmware cannot make for itself -- which folder to treat as the SD card
 * and which SDR backend (and device) to use -- and only then boots the firmware
 * UI. It also carries the launcher's own Start / Quit buttons.
 *
 * The window is drawn in the same 240x320 panel as the firmware, in the
 * PortaPack style, using the firmware's own bitmap font so the port needs no
 * extra text-rendering dependency. It runs entirely before portapack::init(),
 * so the SD folder it returns can still be installed (MAYHEM_SD and the
 * persistent-memory mapping) before any firmware code reads them.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_LAUNCHER_HPP__
#define __HOST_LAUNCHER_HPP__

#include <string>

namespace host_launcher {

struct Result {
    bool start{false};        /* true = Start pressed, false = Quit */
    std::string sd_dir;       /* chosen SD folder */
    std::string driver;       /* "rtlsdr" or "pluto" */
    std::string device_args;  /* driver-specific device hint */
    bool touch_only{false};   /* true = hide the on-screen button deck */
    int rotation{0};          /* body rotation in degrees: 0/90/180/270 */
};

/* Body rotation saved as the default by a previous setup session (degrees,
 * 0/90/180/270). Read at startup so a headless/skip-launcher boot still turns
 * the window the way the user left it. */
int saved_rotation();

/* Shows the setup window and blocks until the user presses Start or Quit (or
 * closes the window, which counts as Quit). `default_sd` seeds the folder
 * field. Must be called after the SDL window exists and before
 * portapack::init(). */
Result run(const std::string& default_sd, int scale);

}  // namespace host_launcher

#endif /*__HOST_LAUNCHER_HPP__*/

/*
 * Host-side settings shared by the SDL input layer and the pre-boot setup window.
 *
 * On the device every control is fixed by the hardware: the five-way cross, the
 * rotary encoder and the touch panel are where they are. The desktop port maps
 * them onto the keyboard and mouse instead, and this module is where that
 * mapping lives so the setup window can let the user change it: which key is
 * each navigation button, which key turns the encoder, which key flips the
 * window and which toggles fullscreen, plus the mouse-wheel direction.
 *
 * Keys are stored as SDL *scancodes*, not keycodes: a scancode names the
 * physical key on the keyboard, so a binding keeps working whatever the
 * keyboard layout (English, Russian, ...) is switched to. Careful: the number
 * is layout-independent but it is defined as a US-layout position, so the label
 * shown for a key is its US name (see sdl layer).
 *
 * Everything is persisted in a tiny INI-ish file next to the rotation setting
 * (see config_file()), so the choices survive a restart. Scancodes are raw ints
 * here so this header stays free of any SDL include.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_SETTINGS_HPP__
#define __HOST_SETTINGS_HPP__

#include <cstdint>
#include <string>

namespace host_settings {

/* The remappable actions. The first six feed the firmware's button bits; the
 * rest are host-only (encoder, window rotation, fullscreen). */
enum class Action : int {
    NavUp = 0,
    NavDown,
    NavLeft,
    NavRight,
    Select,
    Dfu,
    EncoderCW,
    EncoderCCW,
    RotateWindow,
    ToggleFullscreen,
    Count,
};

/* Human-readable name for the setup window's binding list. */
const char* action_label(Action a);

/* Factory default scancode for an action. */
int32_t default_scancode(Action a);

/* Current binding for an action (an SDL scancode). */
int32_t scancode(Action a);

/* Rebinds an action. Does not save; call save() when the edit is finished. */
void set_scancode(Action a, int32_t sdl_scancode);

/* Action bound to an SDL scancode, or Action::Count when the key is unused. */
Action action_for_scancode(int32_t sdl_scancode);

/* True when the action is currently bound to this scancode, treating the two
 * Enter keys (Return and Keypad Enter) as interchangeable for Select. */
bool scancode_is(Action a, int32_t sdl_scancode);

/* Restores every binding to its factory default. */
void reset_key_defaults();

/* Body rotation in degrees (0/90/180/270), the default for the next launch. */
int rotation();
void set_rotation(int degrees);

/* Input mode: true hides the on-screen button deck (touchscreen only). */
bool touch_only();
void set_touch_only(bool on);

/* Battery capacity override in mAh. 0 means "auto": read it from the device
 * (fuel gauge / host power supply). A positive value is used verbatim. */
int battery_mah();
void set_battery_mah(int mah);

/* Mouse-wheel direction for the encoder: false (default) means a wheel-up notch
 * (scroll away) turns the encoder clockwise. */
bool wheel_inverted();
void set_wheel_inverted(bool inverted);

/* Loads / stores the whole settings file. load() is safe to call when the file
 * does not exist (defaults are kept). */
void load();
void save();

/* The settings file path, for diagnostics. */
std::string config_file();

}  // namespace host_settings

#endif /* __HOST_SETTINGS_HPP__ */

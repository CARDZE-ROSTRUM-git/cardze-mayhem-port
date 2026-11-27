/*
 * SDL2 presentation layer for the desktop PortaPack.
 *
 * The firmware draws into an RGB565 surface (host_lcd) exactly as it drives the
 * real ILI9341 panel. This layer renders that surface inside a scaled PortaPack
 * H2 body, and converts keyboard/mouse input into the host_input state that the
 * controls layer samples.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __LCD_SDL_HPP__
#define __LCD_SDL_HPP__

namespace lcd_sdl {

/* Creates the window, renderer and panel texture. `scale` is 1..3. Returns
 * false (with an SDL_Log message) if the video subsystem is unavailable. */
bool init(const char* title, int scale);

/* Tears down every SDL resource. Safe to call after a failed init(). */
void shutdown();

/* Drains the SDL event queue. Returns false once the user closed the window. */
bool pump();

/* True after a quit request (window close) was observed. */
bool quit_requested();

/* True once after the window geometry changed (rotation or scale) and a repaint
 * is still pending, so the main loop can flush a rotation applied to a static
 * screen instead of waiting for the firmware's next frame. */
bool consume_redraw();

/* Repaints the enclosure and uploads the current panel surface. */
void render();

/* Device geometry in device units and the active scale, for tests/diagnostics. */
int device_width();
int device_height();
int scale();

/* Show or hide the physical control deck (the five-way cross and the rotary
 * encoder) below the panel. Hidden means "touchscreen only": the body shrinks
 * so the window is just the screen, keeping the top strip (rotate / FS / DFU)
 * and the silkscreen band with the owner handle. Visible by default. */
void set_deck_visible(bool on);
bool deck_visible();

/* Set the body rotation in degrees (snapped to 0/90/180/270, clockwise) and
 * resize the floating window to match, exactly as pressing R does. The setup
 * window uses this to apply a saved default rotation at startup. */
void set_rotation(int degrees);
int rotation();

/* Injects a synthetic SDL keyboard/mouse event into this process's own event
 * queue. Used by main.cpp for the splash auto-advance and the MAYHEM_SELFTEST
 * path; it never touches the operator's input devices. */
void inject_key_event(int sdl_keycode, bool down);
void inject_mouse_click(int window_x, int window_y);

/* ---------------------------------------------------------------------- */
/* Pre-boot setup (launcher) support.                                       */
/*                                                                          */
/* The setup window runs before the firmware: there is no UI tree and no      */
/* controls scan yet, so it needs raw panel input instead of host_input.      */
/* While launcher mode is on, pump() stops feeding host_input and instead     */
/* queues clicks (mapped to panel pixels), key presses and window-close for   */
/* host_launcher to drain. Scale (1/2/3) and rotation (R) keep working.       */
/* ---------------------------------------------------------------------- */
struct LauncherEvent {
    enum class Type { Click, Release, Key, Quit };
    Type type;
    int x{0};       /* Click/Release: panel pixel X */
    int y{0};       /* Click/Release: panel pixel Y */
    int key{0};     /* Key: SDL_Keycode (layout-dependent symbol) */
    int scancode{0}; /* Key: SDL_Scancode (physical key, layout-independent) */
};

/* Toggle launcher mode on the SDL layer. */
void set_launcher_mode(bool on);

/* Dequeues one setup-window event; false when the queue is empty. */
bool launcher_poll(LauncherEvent& out);

/* Key-capture mode for the setup window's binding editor. While active, the
 * next key press is recorded instead of being delivered as a launcher Key
 * event, so the editor can read the physical scancode it should bind. */
void begin_key_capture();
void cancel_key_capture();
/* True once a key has been captured; fills `scancode` and clears the latch. */
bool poll_captured_scancode(int& scancode);
/* True while a capture is still pending (not yet satisfied or cancelled). */
bool key_capture_active();

/* Layout-independent display name for a scancode ("A", "Up", "Left Ctrl", ...),
 * used by the setup window to label the current binding. */
const char* scancode_name(int scancode);

}  // namespace lcd_sdl

#endif /* __LCD_SDL_HPP__ */

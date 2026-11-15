/*
 * SDL2 window presenting the emulated PortaPack LCD inside a PortaPack H2 body.
 *
 * The firmware paints into the RGB565 surface owned by host_lcd.cpp exactly as
 * it would drive the real ILI9341 panel. This file blits that surface into an
 * SDL texture, inside a scaled, drawn-to-scale H2 enclosure, and turns keyboard
 * and mouse input into the host_input state the controls layer samples.
 *
 * Layout notes:
 *   - The panel is 240x320 in portrait, matching the physical screen.
 *   - The H2 is a portrait hand-held: a top strip carries the two host-only
 *     controls (the square window-rotation button on the left and the DFU
 *     button on the right), then the panel, then a control deck below it with
 *     the rotary encoder on the left and the five-way cross
 *     (d-pad: up/down/left/right + select) on the right, exactly as the real
 *     case does.
 *   - The window-rotation button turns the whole instrument 90 degrees clockwise
 *     on every press (0 -> 90 -> 180 -> 270 -> 0). The entire device -- the top
 *     strip with the two buttons included -- is rendered to an off-screen
 *     texture and blitted rotated, so the panel, the frame and the two buttons
 *     all turn together; the rotation button and DFU therefore travel around the
 *     window with the body.
 *   - Scale x1/x2/x3 (keys 1/2/3) resize the window; the whole device scales
 *     together.
 *   - The five cross buttons, the encoder and the touch panel are the real H2
 *     affordances; the mouse acts as the resistive touchscreen, and the keyboard
 *     mirrors every button.
 *
 * This file owns no firmware logic. It only presents and collects input.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "lcd_sdl.hpp"

#include "host_input.hpp"
#include "host_lcd.hpp"

/* hal.h (force-included) defines __I/__O/__IO as I/O qualifiers for the LPC43xx
 * register block. SDL pulls in <immintrin.h>, whose AVX intrinsics use those
 * same tokens as parameter names, so they must be dropped before SDL is seen.
 * Nothing in this translation unit touches the register headers. */
#undef __I
#undef __O
#undef __IO

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace lcd_sdl {

namespace {

/* ---------------------------------------------------------------------- */
/* Geometry, in "device units" (1 device unit = 1 panel pixel at scale 1).  */
/*                                                                        */
/* The whole instrument is one unit that rotates as a whole:              */
/*   - a top strip carrying the two host-only controls: the square         */
/*     window-rotation button on the left and the DFU button on the right;  */
/*   - the 240x320 panel below it;                                          */
/*   - a control deck underneath carrying the rotary encoder on the left    */
/*     and the five-way cross (d-pad) on the right, exactly as the real H2  */
/*     case does.                                                          */
/*                                                                        */
/* Every part is expressed in device units and drawn into one off-screen    */
/* texture, which is then blitted rotated. The rotation button and DFU      */
/* therefore travel around the window together with the body instead of     */
/* staying pinned to the window corners.                                    */
/* ---------------------------------------------------------------------- */
constexpr int PANEL_W = host_lcd::WIDTH;   /* 240 */
constexpr int PANEL_H = host_lcd::HEIGHT;  /* 320 */

constexpr int BEZEL = 10;    /* bezel around the panel */
constexpr int MARGIN = 6;    /* outer shell border */
constexpr int DECK_H = 96;   /* control deck height below the panel */

/* Top strip with the rotation button and DFU. */
constexpr int HEADER_H = 28;

/* The device is as wide as the panel plus its bezel and shell margins. */
constexpr int DEVICE_W = MARGIN + BEZEL + PANEL_W + BEZEL + MARGIN;

/* Panel origin inside the device, below the top strip. */
constexpr int PANEL_X = MARGIN + BEZEL;
constexpr int PANEL_Y = HEADER_H + MARGIN + BEZEL;

/* Control deck below the panel. */
constexpr int DECK_Y = PANEL_Y + PANEL_H + BEZEL;

/* Bottom silkscreen band carrying the host handle, below the deck. */
constexpr int FOOTER_H = 18;

/* Full device extent, top strip included. */
constexpr int DEVICE_H = DECK_Y + DECK_H + FOOTER_H;

/* The physical control deck (five-way cross + rotary encoder) below the panel
 * can be hidden so the window becomes just the screen ("touchscreen only").
 * When hidden the body is DECK_H shorter; the top strip (rotate / FS / DFU) and
 * the silkscreen band with the owner handle stay. Toggled from the pre-boot
 * setup window through set_deck_visible(). */
bool g_deck_visible = true;

/* Body height for the current deck visibility. Everything that lays out or maps
 * against the body's bottom edge goes through this instead of DEVICE_H. */
inline int dev_h() { return g_deck_visible ? DEVICE_H : DEVICE_H - DECK_H; }

/* H2 body palette. */
constexpr uint32_t COL_SHELL = 0x303030;
constexpr uint32_t COL_SHELL_EDGE = 0x1c1c1c;
constexpr uint32_t COL_BEZEL = 0x0e0e0e;
constexpr uint32_t COL_BTN = 0x505050;
constexpr uint32_t COL_BTN_PRESSED = 0x8fb8d8;
constexpr uint32_t COL_BTN_EDGE = 0x242424;
constexpr uint32_t COL_WHEEL = 0x3a3a3a;
constexpr uint32_t COL_WHEEL_RING = 0x2f6fd0;  /* the knob's blue LED ring */
constexpr uint32_t COL_LABEL = 0xd0d0d0;

struct ButtonBox {
    host_input::Btn btn;
    SDL_Rect rect;  /* device units */
    const char* label;
};

/* The five-way cross on the right of the deck: up, down, left, right and
 * centre/select. The rects are in device units and are filled in by
 * layout_controls(). */
ButtonBox kButtons[] = {
    {host_input::Btn::Up,     {0, 0, 0, 0}, "^"},
    {host_input::Btn::Down,   {0, 0, 0, 0}, "v"},
    {host_input::Btn::Left,   {0, 0, 0, 0}, "<"},
    {host_input::Btn::Right,  {0, 0, 0, 0}, ">"},
    {host_input::Btn::Select, {0, 0, 0, 0}, "o"},
};
constexpr int kCrossCount = 5;

/* Host-only window-rotation button in the top-left of the device's top strip.
 * It is not a firmware button: it only turns the whole instrument 90 degrees
 * clockwise on each press. Its rect is in device units and rotates with the
 * body. */
SDL_Rect g_rotate_rect{};

/* DFU button in the top-right of the device's top strip (device units). It is a
 * real firmware button (host_input::Btn::Dfu) and rotates with the body. */
SDL_Rect g_dfu_rect{};

/* Host-only fullscreen button in the middle of the top strip. Like the rotation
 * square it is not a firmware control: it toggles the desktop window between
 * the floating size it starts in and a borderless fullscreen fit. Rotates with
 * the body. Its rect is in device units. */
SDL_Rect g_fullscreen_rect{};

/* True while the window is in borderless fullscreen. The body is then scaled
 * uniformly (aspect preserved) and centred, so the instrument sits in the middle
 * of the screen instead of being stretched to its edges. */
bool g_fullscreen = false;

int g_rotation = 0;  /* 0, 90, 180 or 270 degrees clockwise */

/* Set whenever the window's own geometry changes (rotation or scale). The main
 * loop otherwise repaints only when the firmware drew a new frame, so without
 * this a rotation applied while the screen is static would not be shown until
 * the next frame lands. consume_redraw() clears it. */
bool g_needs_redraw = false;

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_panel_texture = nullptr;
SDL_Texture* g_device_texture = nullptr;  /* render target for the whole device */
SDL_Texture* g_static_texture = nullptr;  /* baked, constant H2 body (see ensure_static_texture) */
int g_scale = 1;
bool g_quit = false;

/* Last backlight state seen by the presentation loop, so a change (the firmware
 * putting the display to sleep or waking it) forces one repaint even while the
 * firmware paints nothing further. */
bool g_backlight_last = true;

/* Scale used while drawing the body into the off-screen texture. The body is
 * always drawn at 1:1 device units and the finished texture is scaled to fit
 * the window at present time (see render()), so the instrument follows any
 * hand-resize of the window instead of staying at a fixed integer scale. */
constexpr int kDrawScale = 1;

/* Pre-boot setup (launcher) mode. While on, pump() collects panel clicks,
 * keys and quit into a queue for host_launcher instead of feeding the
 * firmware's host_input state (there is no UI tree yet). */
bool g_launcher_mode = false;
std::vector<LauncherEvent> g_launcher_events;
size_t g_launcher_read = 0;

/* Device-unit box for the encoder knob, recomputed per scale. */
SDL_Rect g_wheel_rect{};

/* Mouse-as-touch: which device-unit control the pointer is over, if any. */
host_input::Btn g_pressed_button = host_input::Btn::Select;  /* unused sentinel */
bool g_pressed_button_valid = false;
bool g_wheel_dragging = false;
int g_wheel_acc = 0;  /* accumulated device-space drag while turning the knob */
bool g_wheel_turned = false;  /* did the drag actually turn the encoder? */

/* Minimum hold for synthetic presses.
 *
 * SDL delivers a quick click as a button-down followed immediately by an
 * up, often inside a single pump() drain. The 1 kHz controller scan in the main
 * loop would then sample the state only after both were applied, so the press
 * would never be seen by the debouncers or the touch state machine. To avoid
 * that, every press is held for at least kHoldTicks scan periods; a release is
 * only applied once the minimum hold has elapsed, and tick() counts it down. */
constexpr uint16_t kHoldTicks = 60;

uint16_t g_button_hold[6] = {0, 0, 0, 0, 0, 0};
bool g_button_release[6] = {false, false, false, false, false, false};

uint16_t g_touch_hold = 0;
bool g_touch_release = false;

/* ---------------------------------------------------------------------- */
/* Geometry helpers                                                        */
/* ---------------------------------------------------------------------- */

/* Width (in window device units) of the device as it appears on screen, and its
 * height. At 90/270 degrees the whole instrument is turned on its side, so the
 * on-screen width and height swap. */
inline int window_width_dev() {
    return (g_rotation == 90 || g_rotation == 270) ? dev_h() : DEVICE_W;
}
inline int window_height_dev() {
    return (g_rotation == 90 || g_rotation == 270) ? DEVICE_W : dev_h();
}

/* Window size in pixels for the current scale and rotation. */
void current_window_size(int& w, int& h) {
    const int s = g_scale;
    w = window_width_dev() * s;
    h = window_height_dev() * s;
}

/* On-screen scale actually used for drawing the body into the window.
 *
 * The window is free to be resized by hand (or by the compositor), so rather
 * than a fixed integer scale the whole body fills whatever the window currently
 * is. The two axes are scaled *independently* (s_w along the body's own X, s_h
 * along its Y) so that widening the window widens the instrument, and shrinking
 * it lets the instrument shrink onto a small screen. At the requested integer
 * scale both equal g_scale, so the normal case is unchanged.
 *
 * The window is in the *rotated* frame, so under 90/270 the body's X maps to
 * the window's height and vice versa (SDL scales first, then rotates). */
void current_screen_scale_xy(double& s_w, double& s_h) {
    int ww = DEVICE_W, wh = dev_h();
    if (g_window != nullptr) SDL_GetWindowSize(g_window, &ww, &wh);
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;

    const int rot = ((g_rotation % 360) + 360) % 360;
    if (rot == 90 || rot == 270) {
        s_w = static_cast<double>(wh) / DEVICE_W;
        s_h = static_cast<double>(ww) / dev_h();
    } else {
        s_w = static_cast<double>(ww) / DEVICE_W;
        s_h = static_cast<double>(wh) / dev_h();
    }

    /* In fullscreen the instrument keeps its aspect ratio and sits centred
     * (letterboxed) instead of being stretched to the screen's edges: the same
     * uniform factor is used on both axes, so the body no longer follows a
     * hand-resize there. Mapping input uses this too, so clicks stay exact. */
    if (g_fullscreen) {
        const double s = (s_w < s_h) ? s_w : s_h;
        s_w = s;
        s_h = s;
    }
}

/* Lays out the device controls in device units: the rotation square (top-left
 * of the top strip) and DFU (top-right), the encoder knob on the deck's left and
 * the five-way cross on the deck's right. Everything is in the body's own frame
 * and rotates with it. */
void layout_controls() {
    /* Top strip: rotation square (left) and DFU (right). */
    const int square = 22;
    g_rotate_rect = {8, (HEADER_H - square) / 2, square, square};

    const int dfu_w = 40;
    const int dfu_h = 20;
    g_dfu_rect = {DEVICE_W - 8 - dfu_w, (HEADER_H - dfu_h) / 2, dfu_w, dfu_h};

    /* Fullscreen toggle button, centred in the top strip between the rotation
     * square and DFU. A small host-only control labelled "FS". */
    const int fs_w = 30;
    const int fs_h = 20;
    g_fullscreen_rect = {(DEVICE_W - fs_w) / 2, (HEADER_H - fs_h) / 2, fs_w, fs_h};

    /* Encoder knob: a round cap on the left of the deck. */
    const int wheel_size = 76;
    const int wheel_x = PANEL_X + 14;
    const int wheel_y = DECK_Y + (DECK_H - wheel_size) / 2;
    g_wheel_rect = {wheel_x, wheel_y, wheel_size, wheel_size};

    /* Five-way cross: up / left / select / right / down in a 3x3 grid. */
    const int btn = 30;
    const int gap = 2;
    const int cell = btn + gap;  /* pitch between neighbouring buttons */

    const int right_area = PANEL_X + PANEL_W - 12;
    const int cx = right_area - (btn + 2 * cell) + btn / 2;  /* centre of the cross */
    const int cy = DECK_Y + DECK_H / 2;

    const int centre_x = cx - btn / 2;
    const int centre_y = cy - btn / 2;

    kButtons[0].rect = {centre_x, centre_y - cell, btn, btn};  /* Up */
    kButtons[1].rect = {centre_x, centre_y + cell, btn, btn};  /* Down */
    kButtons[2].rect = {centre_x - cell, centre_y, btn, btn};  /* Left */
    kButtons[3].rect = {centre_x + cell, centre_y, btn, btn};  /* Right */
    kButtons[4].rect = {centre_x, centre_y, btn, btn};         /* Select */
}

/* Maps an on-screen control box (device units) for hit-testing. */
bool point_in(const SDL_Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Maps a window pixel back to device units (dx/dy), undoing the current rotation
 * about the window's centre. The whole instrument turns as one, so a single
 * inverse rotation is all that is needed. Uses the actual (per-axis) on-screen
 * scale so it stays correct after a hand-resize of the window. */
void window_to_device(int wx, int wy, int& dx, int& dy) {
    int cw = 0, ch = 0;
    SDL_GetWindowSize(g_window, &cw, &ch);
    double s_w = 1.0, s_h = 1.0;
    current_screen_scale_xy(s_w, s_h);
    if (s_w <= 0.0 || s_h <= 0.0) { dx = 0; dy = 0; return; }

    /* Offset of the point from the window centre. */
    const double U = wx - cw / 2.0;
    const double V = wy - ch / 2.0;

    /* Rotate the offset back by -g_rotation (screen y points down) into the
     * body's own un-rotated image space, in window pixels. */
    double u, v;
    switch (((g_rotation % 360) + 360) % 360) {
        case 90:  u = V;  v = -U; break;
        case 180: u = -U; v = -V; break;
        case 270: u = -V; v = U;  break;
        default:  u = U;  v = V;  break;
    }

    /* Convert to device units about the body's centre, per axis. */
    dx = static_cast<int>(u / s_w + DEVICE_W / 2.0);
    dy = static_cast<int>(v / s_h + dev_h() / 2.0);
}

/* Rotates a window-space motion delta into the device's own axes, undoing
 * g_rotation, so a drag that follows the (rotated) knob still reads as a
 * vertical gesture in device space. */
void device_delta(int wx, int wy, int& ddx, int& ddy) {
    switch (((g_rotation % 360) + 360) % 360) {
        case 90:  ddx = wy;  ddy = -wx; break;
        case 180: ddx = -wx; ddy = -wy; break;
        case 270: ddx = -wy; ddy = wx;  break;
        default:  ddx = wx;  ddy = wy;  break;
    }
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                 */
/* ---------------------------------------------------------------------- */

void fill(SDL_Renderer* r, const SDL_Rect& rect, uint32_t rgb, int scale) {
    SDL_SetRenderDrawColor(r, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff);
    SDL_Rect s{rect.x * scale, rect.y * scale, rect.w * scale, rect.h * scale};
    SDL_RenderFillRect(r, &s);
}

uint32_t shade(uint32_t rgb, double f) {
    auto ch = [&](int shift) {
        int v = static_cast<int>(((rgb >> shift) & 0xff) * f);
        return std::clamp(v, 0, 255);
    };
    return (static_cast<uint32_t>(ch(16)) << 16) |
           (static_cast<uint32_t>(ch(8)) << 8) |
           static_cast<uint32_t>(ch(0));
}

/* ---------------------------------------------------------------------- */
/* Minimal 5x7 label font (uppercase + digits + a little punctuation).     */
/* The on-screen buttons are the only text this layer draws, so a compact   */
/* fixed font keeps the port free of extra dependencies (e.g. SDL_ttf).     */
/* ---------------------------------------------------------------------- */

/* Each glyph is 5 columns; bit0 is the top row. Rows 0..6. */
struct Glyph {
    char ch;
    uint8_t col[5];
};

const Glyph kGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
    {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
    {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
    {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
    {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x7f, 0x20, 0x18, 0x20, 0x7f}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    /* Direction arrows for the five-way cross labels. These are real glyphs,
     * not letters: 'v' must not be folded to 'V'. */
    {'^', {0x04, 0x06, 0x3f, 0x06, 0x04}},
    {'v', {0x08, 0x18, 0x3f, 0x18, 0x08}},
    {'<', {0x08, 0x08, 0x1c, 0x3e, 0x08}},
    {'>', {0x08, 0x3e, 0x1c, 0x08, 0x08}},
    /* Punctuation used by the owner handle drawn on the deck. */
    {'_', {0x40, 0x40, 0x40, 0x40, 0x40}},
    {'@', {0x3e, 0x41, 0x5d, 0x55, 0x1e}},
};

/* True for the arrow label glyphs, which must be matched verbatim rather than
 * case-folded into letters. */
bool is_arrow(char c) {
    return c == '^' || c == 'v' || c == '<' || c == '>';
}

const Glyph* find_glyph(char c) {
    if (!is_arrow(c) && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const auto& g : kGlyphs) {
        if (g.ch == c) return &g;
    }
    return &kGlyphs[0]; /* space */
}

int text_width(const char* text, int scale) {
    const int len = static_cast<int>(std::strlen(text));
    return (len > 0) ? (len * 6 - 1) * scale : 0;
}

void draw_text(SDL_Renderer* r, int x, int y, const char* text, uint32_t rgb, int scale) {
    SDL_SetRenderDrawColor(r, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff);
    /* x/y/scale are in device units; the body is drawn 1:1 and scaled later. */
    const int s = scale * kDrawScale;
    int cx = x * kDrawScale;
    const int cy = y * kDrawScale;
    for (const char* p = text; *p; ++p) {
        const Glyph* g = find_glyph(*p);
        for (int col = 0; col < 5; col++) {
            const uint8_t bits = g->col[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1U << row)) {
                    SDL_Rect px{cx + col * s, cy + row * s, s, s};
                    SDL_RenderFillRect(r, &px);
                }
            }
        }
        cx += 6 * s;
    }
}

/* Draws a small clockwise-rotation icon (a curved arrow) centred in `box`. */
void draw_rotate_icon(SDL_Renderer* r, const SDL_Rect& box, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff);
    const int s = kDrawScale;
    const int cx = box.x + box.w / 2;
    const int cy = box.y + box.h / 2;
    const int rad = box.w / 2 - 5;

    /* Open ring (leave a gap on the right for the arrow head). */
    for (int dy = -rad; dy <= rad; dy++) {
        for (int dx = -rad; dx <= rad; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 <= rad * rad && d2 >= (rad - 2) * (rad - 2)) {
                if (dx > 2) continue;  /* gap */
                SDL_Rect px{(cx + dx) * s, (cy + dy) * s, s, s};
                SDL_RenderFillRect(r, &px);
            }
        }
    }
    /* Arrow head pointing up-right at the gap. */
    fill(r, {cx + 2, cy - rad - 1, 3, 3}, rgb, kDrawScale);
    fill(r, {cx + 4, cy - rad + 1, 2, 4}, rgb, kDrawScale);
}

/* Draws the *static* parts of the body into the current render target, in body
 * device units: (0,0) is the body's top-left corner.
 *
 * These pixels never depend on a button being pressed and never change shape,
 * so the result is baked into an off-screen texture once (see
 * ensure_static_texture) and blitted every frame. Drawing them afresh each frame
 * cost thousands of SDL_RenderFillRect calls (the encoder's LED ring alone is
 * ~4500 per frame), which was the main source of the frame-time spikes. The
 * pressed-state visuals are re-drawn on top by draw_pressed_overlays(). */
void draw_static_body(SDL_Renderer* r) {
    /* Shell, covering the whole instrument (top strip + panel + deck). */
    fill(r, {0, 0, DEVICE_W, dev_h()}, COL_SHELL, kDrawScale);

    /* Top strip (slightly darker), carrying the rotate and DFU buttons. */
    fill(r, {0, 0, DEVICE_W, HEADER_H}, shade(COL_SHELL, 0.7), kDrawScale);
    fill(r, {0, HEADER_H - 1, DEVICE_W, 1}, COL_SHELL_EDGE, kDrawScale);

    /* Window-rotation square (top-left of the strip). */
    fill(r, g_rotate_rect, COL_BTN, kDrawScale);
    fill(r, {g_rotate_rect.x, g_rotate_rect.y, g_rotate_rect.w, 2}, COL_BTN_EDGE, kDrawScale);
    fill(r, {g_rotate_rect.x, g_rotate_rect.y + g_rotate_rect.h - 2, g_rotate_rect.w, 2},
         COL_BTN_EDGE, kDrawScale);
    draw_rotate_icon(r, g_rotate_rect, COL_LABEL);

    /* DFU button (top-right of the strip), always in the unpressed state here;
     * a press is painted by draw_pressed_overlays(). */
    fill(r, g_dfu_rect, COL_BTN, kDrawScale);
    fill(r, {g_dfu_rect.x, g_dfu_rect.y, g_dfu_rect.w, 2}, COL_BTN_EDGE, kDrawScale);
    fill(r, {g_dfu_rect.x, g_dfu_rect.y + g_dfu_rect.h - 2, g_dfu_rect.w, 2},
         COL_BTN_EDGE, kDrawScale);
    {
        const int tw = text_width("DFU", 1);
        draw_text(r, g_dfu_rect.x + (g_dfu_rect.w - tw) / 2,
                  g_dfu_rect.y + (g_dfu_rect.h - 7) / 2,
                  "DFU", COL_LABEL, 1);
    }

    /* Fullscreen toggle button (host-only), centred in the strip. */
    fill(r, g_fullscreen_rect, COL_BTN, kDrawScale);
    fill(r, {g_fullscreen_rect.x, g_fullscreen_rect.y, g_fullscreen_rect.w, 2}, COL_BTN_EDGE, kDrawScale);
    fill(r, {g_fullscreen_rect.x, g_fullscreen_rect.y + g_fullscreen_rect.h - 2, g_fullscreen_rect.w, 2},
         COL_BTN_EDGE, kDrawScale);
    {
        const int tw = text_width("FS", 1);
        draw_text(r, g_fullscreen_rect.x + (g_fullscreen_rect.w - tw) / 2,
                  g_fullscreen_rect.y + (g_fullscreen_rect.h - 7) / 2,
                  "FS", COL_LABEL, 1);
    }

    /* Panel bezel and the LCD itself (the texture is drawn later). */
    fill(r, {PANEL_X - BEZEL, PANEL_Y - BEZEL, PANEL_W + 2 * BEZEL, PANEL_H + 2 * BEZEL},
         COL_BEZEL, kDrawScale);

    /* Control deck below the panel (slightly darker than the shell). Hidden in
     * "touchscreen only" mode, which is the whole point of dev_h() shrinking. */
    if (g_deck_visible) {
        fill(r, {MARGIN, DECK_Y, DEVICE_W - 2 * MARGIN, DECK_H},
             shade(COL_SHELL, 0.82), kDrawScale);
    }

    /* Bottom silkscreen band, a touch darker still, with a separator line. */
    fill(r, {0, dev_h() - FOOTER_H, DEVICE_W, FOOTER_H},
         shade(COL_SHELL, 0.72), kDrawScale);
    fill(r, {0, dev_h() - FOOTER_H, DEVICE_W, 1}, COL_SHELL_EDGE, kDrawScale);

    /* Encoder knob: a round cap with a blue LED ring around it. */
    if (g_deck_visible) {
    const int wcx = g_wheel_rect.x + g_wheel_rect.w / 2;
    const int wcy = g_wheel_rect.y + g_wheel_rect.h / 2;
    const int wrad = g_wheel_rect.w / 2;

    SDL_SetRenderDrawColor(r, (COL_WHEEL_RING >> 16) & 0xff, (COL_WHEEL_RING >> 8) & 0xff,
                           COL_WHEEL_RING & 0xff, 0xff);
    for (int dy = -wrad; dy <= wrad; dy++) {
        for (int dx = -wrad; dx <= wrad; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 <= wrad * wrad && d2 >= (wrad - 3) * (wrad - 3)) {
                SDL_Rect px{(wcx + dx) * kDrawScale, (wcy + dy) * kDrawScale, kDrawScale, kDrawScale};
                SDL_RenderFillRect(r, &px);
            }
        }
    }
    SDL_SetRenderDrawColor(r, (COL_WHEEL >> 16) & 0xff, (COL_WHEEL >> 8) & 0xff,
                           COL_WHEEL & 0xff, 0xff);
    for (int dy = -wrad + 3; dy <= wrad - 3; dy++) {
        for (int dx = -wrad + 3; dx <= wrad - 3; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 <= (wrad - 3) * (wrad - 3)) {
                SDL_Rect px{(wcx + dx) * kDrawScale, (wcy + dy) * kDrawScale, kDrawScale, kDrawScale};
                SDL_RenderFillRect(r, &px);
            }
        }
    }
    }

    /* The five-way cross, always in the unpressed state here; a press is painted
     * by draw_pressed_overlays(). Its centre is drawn last so it overlaps. */
    if (g_deck_visible) {
    for (const auto& b : kButtons) {
        fill(r, b.rect, COL_BTN, kDrawScale);
        fill(r, {b.rect.x, b.rect.y, b.rect.w, 2}, COL_BTN_EDGE, kDrawScale);
        fill(r, {b.rect.x, b.rect.y + b.rect.h - 2, b.rect.w, 2}, COL_BTN_EDGE, kDrawScale);

        const int tw = text_width(b.label, 1);
        draw_text(r, b.rect.x + (b.rect.w - tw) / 2, b.rect.y + (b.rect.h - 7) / 2,
                  b.label, COL_LABEL, 1);
    }
    }

    /* Owner handle, pixel letters, right-aligned on the bottom silkscreen
     * band (host-only branding, like the print on a real PortaPack). Static,
     * so it bakes into the body texture together with the rest of the shell. */
    {
        const char* handle = "@CARDPUTER_ZERO";
        const int hw = text_width(handle, 1);
        const int hy = dev_h() - FOOTER_H + (FOOTER_H - 7) / 2;
        draw_text(r, DEVICE_W - MARGIN - 2 - hw, hy, handle,
                  shade(COL_LABEL, 0.6), 1);
    }

    /* Shell inner edge for depth. */
    SDL_SetRenderDrawColor(r, 0x1c, 0x1c, 0x1c, 0xff);
    SDL_Rect edge{1 * kDrawScale, 1 * kDrawScale, (DEVICE_W - 2) * kDrawScale, (dev_h() - 2) * kDrawScale};
    SDL_RenderDrawRect(r, &edge);
}

/* Re-draws only the control that is currently pressed, on top of the baked
 * static body. Empty (and therefore nearly free) when nothing is held, which is
 * the common case. The pressed fill covers the unpressed one exactly and the
 * pressed label sits on top of the unpressed label, so no erasing is needed. */
void draw_pressed_overlays(SDL_Renderer* r) {
    if (!g_pressed_button_valid) return;
    const host_input::Btn b = g_pressed_button;

    const auto paint = [r](const SDL_Rect& rect, const char* text) {
        fill(r, rect, COL_BTN_PRESSED, kDrawScale);
        const int tw = text_width(text, 1);
        draw_text(r, rect.x + (rect.w - tw) / 2, rect.y + (rect.h - 7) / 2,
                  text, 0x101010, 1);
    };

    if (b == host_input::Btn::Dfu) {
        paint(g_dfu_rect, "DFU");
        return;
    }
    if (!g_deck_visible) return;
    for (const auto& box : kButtons) {
        if (box.btn == b) {
            paint(box.rect, box.label);
            return;
        }
    }
}

/* Draws the rotating panel contents into the body target, on top of the bezel. */
void draw_panel(SDL_Renderer* r) {
    /* host_lcd stores display-oriented RGB565. Upload it as ARGB8888.
     *
     * The five-bit/six-bit channel expansion is a pure function of the 16-bit
     * source value, so it is precomputed once into a 65536-entry table instead
     * of recomputed per pixel on every frame (240*320 = 76800 expansions per
     * frame otherwise). Rebuilding the table is done lazily so the very first
     * frame is still correct. */
    static uint32_t lut[1 << 16];
    static bool lut_ready = false;
    if (!lut_ready) {
        for (uint32_t p = 0; p < (1U << 16); p++) {
            const uint32_t rr = (p >> 11) & 0x1f;
            const uint32_t gg = (p >> 5) & 0x3f;
            const uint32_t bb = p & 0x1f;
            const uint32_t r8 = (rr << 3) | (rr >> 2);
            const uint32_t g8 = (gg << 2) | (gg >> 4);
            const uint32_t b8 = (bb << 3) | (bb >> 2);
            lut[p] = 0xff000000u | (r8 << 16) | (g8 << 8) | b8;
        }
        lut_ready = true;
    }

    const uint16_t* src = host_lcd::pixels();
    static uint32_t argb[PANEL_W * PANEL_H];

    /* Screen asleep: the firmware turned the backlight off (Sleep button or the
     * backlight timer). The panel is dark; paint it black so the desktop shows
     * the sleep state too. Any key/tap/wheel wakes it (see host_wake_display). */
    if (!host_lcd::backlight_on()) {
        for (size_t i = 0; i < PANEL_W * PANEL_H; i++) argb[i] = 0xff000000u;
        SDL_UpdateTexture(g_panel_texture, nullptr, argb, PANEL_W * sizeof(uint32_t));
        SDL_Rect dst{PANEL_X * kDrawScale, PANEL_Y * kDrawScale, PANEL_W * kDrawScale, PANEL_H * kDrawScale};
        SDL_RenderCopy(r, g_panel_texture, nullptr, &dst);
        return;
    }

    /* Reproduce the panel's scan-out: when a vertical scroll region is
     * programmed (VSCRDEF/VSCRSADD), each display row is read from a frame-
     * memory row offset by the scroll pointer. For everything that does not
     * scroll this is the identity row, so it costs one integer op per line. */
    for (int y = 0; y < PANEL_H; y++) {
        const uint16_t* srow = src + static_cast<size_t>(host_lcd::display_row_source(y)) * PANEL_W;
        uint32_t* drow = argb + static_cast<size_t>(y) * PANEL_W;
        for (int x = 0; x < PANEL_W; x++) {
            drow[x] = lut[srow[x]];
        }
    }

    SDL_UpdateTexture(g_panel_texture, nullptr, argb, PANEL_W * sizeof(uint32_t));

    SDL_Rect dst{PANEL_X * kDrawScale, PANEL_Y * kDrawScale, PANEL_W * kDrawScale, PANEL_H * kDrawScale};
    SDL_RenderCopy(r, g_panel_texture, nullptr, &dst);
}

/* ---------------------------------------------------------------------- */
/* Input                                                                   */
/* ---------------------------------------------------------------------- */

/* Applies a press immediately; a release is deferred until the press has been
 * held for the minimum hold time (see kHoldTicks). This is what makes a quick
 * click visible to the ~1 kHz controller scan and to the touch state machine,
 * which both require the input to persist across several samples. */
void set_button(host_input::Btn b, bool down) {
    const int i = static_cast<int>(b);
    if (down) {
        host_input::state.set(b, true);
        g_button_hold[i] = kHoldTicks;
        g_button_release[i] = false;
        g_pressed_button = b;
        g_pressed_button_valid = true;
    } else {
        if (g_button_hold[i] > 0) {
            g_button_release[i] = true;  /* defer until the minimum hold elapses */
        } else {
            host_input::state.set(b, false);
            g_button_release[i] = false;
        }
        if (g_pressed_button_valid && g_pressed_button == b) {
            g_pressed_button_valid = false;
        }
    }
}

/* Same minimum-hold policy for the touch panel. */
void set_touch(bool down) {
    if (down) {
        host_input::state.touch = true;
        g_touch_hold = kHoldTicks;
        g_touch_release = false;
    } else {
        if (g_touch_hold > 0) {
            g_touch_release = true;
        } else {
            host_input::state.touch = false;
            g_touch_release = false;
        }
    }
}

/* Counts down the minimum-hold timers; called once per scan period from the
 * main loop. A deferred release is applied as soon as its hold expires. */
void tick() {
    for (int i = 0; i < 6; i++) {
        if (g_button_hold[i] > 0) {
            g_button_hold[i]--;
            if (g_button_hold[i] == 0 && g_button_release[i]) {
                host_input::state.set(static_cast<host_input::Btn>(i), false);
                g_button_release[i] = false;
            }
        }
    }
    if (g_touch_hold > 0) {
        g_touch_hold--;
        if (g_touch_hold == 0 && g_touch_release) {
            host_input::state.touch = false;
            g_touch_release = false;
        }
    }
}

/* Maps an SDL keyboard key to a host button, if any. */
bool key_to_button(SDL_Keycode key, host_input::Btn& out) {
    switch (key) {
        case SDLK_UP: out = host_input::Btn::Up; return true;
        case SDLK_DOWN: out = host_input::Btn::Down; return true;
        case SDLK_LEFT: out = host_input::Btn::Left; return true;
        case SDLK_RIGHT: out = host_input::Btn::Right; return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: out = host_input::Btn::Select; return true;
        case SDLK_ESCAPE: out = host_input::Btn::Dfu; return true;
        default: return false;
    }
}

void apply_scale(int scale, bool center_pointer) {
    (void)center_pointer;
    g_scale = std::clamp(scale, 1, 3);
    layout_controls();

    /* While fullscreen the window size is fixed by the compositor; the scale
     * only takes effect once the window floats again. */
    if (!g_fullscreen) {
        int w, h;
        current_window_size(w, h);
        SDL_SetWindowSize(g_window, w, h);
    }
    g_needs_redraw = true;
}

/* Turns the whole window 90 degrees clockwise and resizes it to match. The
 * header controls are laid out in window units, which depend on the rotation, so
 * they are re-laid-out here. */
void rotate_window() {
    g_rotation = (g_rotation + 90) % 360;
    layout_controls();

    /* While fullscreen the window is owned by the compositor and must not be
     * resized; only the rotation inside it changes (render() recentres the
     * body). */
    if (!g_fullscreen) {
        int w, h;
        current_window_size(w, h);
        SDL_SetWindowSize(g_window, w, h);
    }
    g_needs_redraw = true;
}

/* Toggles between the floating window the program starts in and a borderless
 * fullscreen fit. In fullscreen the body keeps its aspect ratio and is centred
 * by render() (letterboxed) rather than stretched, so the instrument sits in the
 * middle of the screen. */
void toggle_fullscreen() {
    g_fullscreen = !g_fullscreen;
    if (g_fullscreen) {
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(g_window, 0);
        /* The compositor may hand the window back at the screen size, so restore
         * the floating size for the requested integer scale. */
        int w, h;
        current_window_size(w, h);
        SDL_SetWindowSize(g_window, w, h);
    }
    g_needs_redraw = true;
}

void handle_event(const SDL_Event& e) {
    /* Pre-boot setup: route raw panel clicks, keys and quit to the launcher
     * queue. Scale (1/2/3) and rotation (R) still apply so the user can size and
     * turn the window while the setup screen is up. */
    if (g_launcher_mode) {
        switch (e.type) {
            case SDL_QUIT:
                g_quit = true;
                g_launcher_events.push_back({LauncherEvent::Type::Quit, 0, 0, 0});
                break;

            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                if (e.key.keysym.sym == SDLK_1) { apply_scale(1, true); break; }
                if (e.key.keysym.sym == SDLK_2) { apply_scale(2, true); break; }
                if (e.key.keysym.sym == SDLK_3) { apply_scale(3, true); break; }
                if (e.key.keysym.sym == SDLK_r) { rotate_window(); break; }
                if (e.key.keysym.sym == SDLK_f || e.key.keysym.sym == SDLK_F11) {
                    toggle_fullscreen();
                    break;
                }
                g_launcher_events.push_back(
                    {LauncherEvent::Type::Key, 0, 0, static_cast<int>(e.key.keysym.sym)});
                break;

            case SDL_MOUSEBUTTONDOWN: {
                if (e.button.button != SDL_BUTTON_LEFT) break;
                int dx = 0, dy = 0;
                window_to_device(e.button.x, e.button.y, dx, dy);
                /* The host-only top-strip toggles still work on the setup screen:
                 * fullscreen and rotation. */
                if (point_in(g_fullscreen_rect, dx, dy)) {
                    toggle_fullscreen();
                    break;
                }
                if (point_in(g_rotate_rect, dx, dy)) {
                    rotate_window();
                    break;
                }
                if (dx >= PANEL_X && dx < PANEL_X + PANEL_W &&
                    dy >= PANEL_Y && dy < PANEL_Y + PANEL_H) {
                    g_launcher_events.push_back(
                        {LauncherEvent::Type::Click, dx - PANEL_X, dy - PANEL_Y, 0});
                }
                break;
            }

            default:
                break;
        }
        return;
    }

    switch (e.type) {
        case SDL_QUIT:
            g_quit = true;
            break;

        /* The compositor may apply the resize a moment later than
         * SDL_SetWindowSize(), or the user may resize by hand. Any geometry
         * change forces one repaint so the rotated body always fills the new
         * window. */
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_needs_redraw = true;
            }
            break;

        case SDL_KEYDOWN: {
            if (e.key.repeat) break;
            /* Any key wakes a sleeping display (the desktop allows this for every
             * input, not just the navigation keys the device uses). The waking
             * key press is swallowed so it does not also act on the UI. The
             * matching KEYUP still arrives and harmlessly releases nothing. */
            if (host_wake_display()) break;
            /* Scale selection. */
            if (e.key.keysym.sym == SDLK_1) { apply_scale(1, true); break; }
            if (e.key.keysym.sym == SDLK_2) { apply_scale(2, true); break; }
            if (e.key.keysym.sym == SDLK_3) { apply_scale(3, true); break; }
            /* Encoder: Q/E rotate counter-clockwise / clockwise. */
            if (e.key.keysym.sym == SDLK_q) { host_input::state.rotate(-1); break; }
            if (e.key.keysym.sym == SDLK_e) { host_input::state.rotate(+1); break; }
            /* Window rotation: R turns the body 90 degrees clockwise, mirroring
             * the on-screen square button in the top-left corner. */
            if (e.key.keysym.sym == SDLK_r) { rotate_window(); break; }
            /* Fullscreen toggle, mirroring the on-screen "FS" button. */
            if (e.key.keysym.sym == SDLK_f || e.key.keysym.sym == SDLK_F11) {
                toggle_fullscreen();
                break;
            }

            host_input::Btn b;
            if (key_to_button(e.key.keysym.sym, b)) set_button(b, true);
            break;
        }

        case SDL_KEYUP: {
            host_input::Btn b;
            if (key_to_button(e.key.keysym.sym, b)) set_button(b, false);
            break;
        }

        case SDL_MOUSEBUTTONDOWN: {
            if (e.button.button == SDL_BUTTON_LEFT) {
                /* A click wakes a sleeping display, wherever it lands. The
                 * waking click is swallowed: it must not also press a button or
                 * deliver a touch, so the user does not accidentally activate
                 * whatever was hidden behind the tap. The matching BUTTONUP is
                 * harmless (nothing is held). */
                if (host_wake_display()) break;
                /* The whole instrument rotates as one, so a click is first
                 * mapped from window pixels back to device units. */
                int dx, dy;
                window_to_device(e.button.x, e.button.y, dx, dy);

                /* Host-only: the window-rotation square in the top strip. */
                if (point_in(g_rotate_rect, dx, dy)) {
                    rotate_window();
                    break;
                }
                /* Host-only: the fullscreen toggle in the middle of the strip. */
                if (point_in(g_fullscreen_rect, dx, dy)) {
                    toggle_fullscreen();
                    break;
                }
                /* DFU button in the top strip. */
                if (point_in(g_dfu_rect, dx, dy)) {
                    set_button(host_input::Btn::Dfu, true);
                    break;
                }

                if (point_in(g_wheel_rect, dx, dy)) {
                    g_wheel_dragging = true;
                    g_wheel_acc = 0;
                    g_wheel_turned = false;
                    break;
                }
                for (const auto& b : kButtons) {
                    if (point_in(b.rect, dx, dy)) {
                        set_button(b.btn, true);
                        break;
                    }
                }
                /* Otherwise the click is a touch on the panel. */
                if (dx >= PANEL_X && dx < PANEL_X + PANEL_W &&
                    dy >= PANEL_Y && dy < PANEL_Y + PANEL_H) {
                    host_input::state.touch_x = dx - PANEL_X;
                    host_input::state.touch_y = dy - PANEL_Y;
                    set_touch(true);
                }
            }
            break;
        }

        case SDL_MOUSEBUTTONUP: {
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (g_wheel_dragging) {
                    /* A press on the knob that never turned it is a click: treat
                     * it as the centre/select button, like the H2 knob's own
                     * click on later revisions. A drag that rotated the encoder
                     * is consumed as rotation only. */
                    if (!g_wheel_turned) {
                        set_button(host_input::Btn::Select, true);
                        set_button(host_input::Btn::Select, false);
                    }
                    g_wheel_dragging = false;
                    break;
                }
                if (g_pressed_button_valid) set_button(g_pressed_button, false);
                g_pressed_button_valid = false;
                set_touch(false);
            }
            break;
        }

        case SDL_MOUSEMOTION: {
            if (g_wheel_dragging) {
                /* The knob turns with the body, so follow the drag along the
                 * device's own vertical axis: map the window-space delta into
                 * device space and accumulate it, emitting a detent every 8
                 * device pixels. */
                int ddx = 0, ddy = 0;
                device_delta(e.motion.xrel, e.motion.yrel, ddx, ddy);
                g_wheel_acc += ddy;
                while (g_wheel_acc <= -8) {
                    host_input::state.rotate(+1);
                    g_wheel_acc += 8;
                    g_wheel_turned = true;
                }
                while (g_wheel_acc >= 8) {
                    host_input::state.rotate(-1);
                    g_wheel_acc -= 8;
                    g_wheel_turned = true;
                }
                break;
            }
            if (host_input::state.touch) {
                int dx, dy;
                window_to_device(e.motion.x, e.motion.y, dx, dy);
                host_input::state.touch_x = dx - PANEL_X;
                host_input::state.touch_y = dy - PANEL_Y;
            }
            break;
        }

        case SDL_MOUSEWHEEL: {
            /* Turning the wheel wakes a sleeping display. The waking notch is
             * swallowed so it does not also move the selection or step the
             * encoder the moment the screen comes back. */
            if (host_wake_display()) break;
            /* The compositor may scale the wheel step (niri's scroll-factor
             * makes one physical notch arrive as preciseY = 1.2), so the
             * integer y comes out as 0 or 2 for consecutive notches and the
             * menu appears to skip or stall a cell. Derive the whole notches
             * from the precise delta instead, so one notch is one menu cell
             * regardless of the scroll factor. */
            const double dy = (e.wheel.preciseY != 0.0f)
                                  ? static_cast<double>(e.wheel.preciseY)
                                  : static_cast<double>(e.wheel.y);
            if (dy != 0.0) {
                int mag = static_cast<int>(std::fabs(dy) + 0.5);
                if (mag < 1) mag = 1;
                host_input::state.rotate(dy > 0.0 ? mag : -mag);
            }
            break;
        }

        default:
            break;
    }
}

}  // namespace

bool init(const char* title, int scale) {
    /* Give the window a stable application id / class on every desktop, so a
     * window-manager rule can match it no matter where the binary lives. On
     * Wayland this becomes the surface's app_id; on X11 the WM_CLASS. On
     * platforms that do not use them (Windows, macOS) the hints are inert, so
     * the same binary stays portable. Must be set before SDL_Init(), which is
     * what reads them when the video backend is brought up. */
#ifdef SDL_HINT_APP_ID
    SDL_SetHint(SDL_HINT_APP_ID, "mayhem-desktop");
#endif
#ifdef SDL_HINT_VIDEO_X11_WMCLASS
    SDL_SetHint(SDL_HINT_VIDEO_X11_WMCLASS, "mayhem-desktop");
#endif
    SDL_SetHint(SDL_HINT_APP_NAME, "PortaPack Mayhem");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    g_scale = std::clamp(scale, 1, 3);
    layout_controls();

    int win_w = 0, win_h = 0;
    current_window_size(win_w, win_h);

    g_window = SDL_CreateWindow(title,
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                win_w, win_h,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    /* No PRESENTVSYNC: the main loop also runs the ~1 kHz input scan, and a
     * vsync-blocking present would stall it to ~60 Hz whenever a frame was
     * ready. The upstream debounce/hold timers count scan periods (1 kHz on the
     * device), so a 60 Hz loop stretched a 60-tick minimum hold to a full
     * second and swallowed the next key press (very visible in the games).
     * Frame pacing comes from the firmware itself, which paints at ~60 Hz and
     * only then raises the dirty flag, so presentation is naturally throttled
     * without vsync. */
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        /* Fall back to software rasterisation when there is no GPU driver. */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    g_panel_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
    if (!g_panel_texture) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        return false;
    }

    /* Keep the minimum tiny: the window is resized on every rotation, and at
     * 90/270 degrees the body is turned on its side, so the required height
     * shrinks well below the startup height. Pinning the minimum to the startup
     * size would stop the window from ever getting shorter, leaving the rotated
     * body floating in a tall, half-empty window. A small fixed floor lets the
     * window follow the rotated geometry exactly. */
    SDL_SetWindowMinimumSize(g_window, 64, 64);
    return true;
}

void shutdown() {
    if (g_static_texture) SDL_DestroyTexture(g_static_texture);
    if (g_device_texture) SDL_DestroyTexture(g_device_texture);
    if (g_panel_texture) SDL_DestroyTexture(g_panel_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_static_texture = nullptr;
    g_device_texture = nullptr;
    g_panel_texture = nullptr;
    g_renderer = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

bool pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        handle_event(e);
    }
    /* Advance the minimum-hold timers once per scan period. */
    tick();

    /* The firmware may turn the backlight off/on without painting anything
     * further (Sleep button, backlight timer, or a wake from host_wake_display).
     * Force one repaint on the transition so the panel actually blanks/wakes. */
    const bool backlight = host_lcd::backlight_on();
    if (backlight != g_backlight_last) {
        g_backlight_last = backlight;
        g_needs_redraw = true;
    }

    return !g_quit;
}

bool quit_requested() {
    return g_quit;
}

/* (Re)creates the off-screen render target that holds one whole device frame at
 * 1:1 device units. Rendering the instrument into a texture lets the final blit
 * be rotated and scaled to the window's present size without re-drawing the
 * frame at an angle. */
void ensure_device_texture() {
    const int w = DEVICE_W * kDrawScale;
    const int h = dev_h() * kDrawScale;
    if (g_device_texture) {
        int tw = 0, th = 0;
        SDL_QueryTexture(g_device_texture, nullptr, nullptr, &tw, &th);
        if (tw == w && th == h) return;
        SDL_DestroyTexture(g_device_texture);
        g_device_texture = nullptr;
    }
    g_device_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_TARGET, w, h);
}

/* (Re)creates the baked body texture. It depends only on the geometry (and the
 * rotation/scale, which change that geometry), so it is rebuilt only when the
 * device extent changes and reused for every frame in between. Callers must
 * have a valid g_renderer. */
void ensure_static_texture() {
    const int w = DEVICE_W * kDrawScale;
    const int h = dev_h() * kDrawScale;
    if (g_static_texture) {
        int tw = 0, th = 0;
        SDL_QueryTexture(g_static_texture, nullptr, nullptr, &tw, &th);
        if (tw == w && th == h) return;
        SDL_DestroyTexture(g_static_texture);
        g_static_texture = nullptr;
    }
    g_static_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_TARGET, w, h);
    if (!g_static_texture) return;

    SDL_SetRenderTarget(g_renderer, g_static_texture);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
    SDL_RenderClear(g_renderer);
    draw_static_body(g_renderer);
    SDL_SetRenderTarget(g_renderer, nullptr);
}

void render() {
    if (!g_renderer) return;

    ensure_device_texture();
    if (!g_device_texture) return;
    ensure_static_texture();
    if (!g_static_texture) return;

    /* Compose one whole instrument frame (top strip + shell + deck + panel) into
     * the off-screen target, at 1:1 device units. The constant parts of the body
     * come from the baked texture; only the panel and the pressed-button
     * highlights are drawn live. */
    SDL_SetRenderTarget(g_renderer, g_device_texture);
    SDL_RenderCopy(g_renderer, g_static_texture, nullptr, nullptr);
    draw_pressed_overlays(g_renderer);
    draw_panel(g_renderer);
    SDL_SetRenderTarget(g_renderer, nullptr);

    /* Stretch the finished body to fill the window's present size, scaling the
     * two axes independently so widening the window widens the instrument (and
     * shrinking it, on a small screen, shrinks the instrument). At the requested
     * integer scale both factors equal g_scale, so the normal case is unchanged.
     *
     * SDL scales the un-rotated image into `dst` and then rotates it about dst's
     * centre, so the body's own X uses s_w and its Y uses s_h; the rotated
     * footprint stays centred in the window at every angle. */
    double s_w = 1.0, s_h = 1.0;
    current_screen_scale_xy(s_w, s_h);
    const int body_w = static_cast<int>(DEVICE_W * s_w + 0.5);
    const int body_h = static_cast<int>(DEVICE_H * s_h + 0.5);
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);

    SDL_SetRenderDrawColor(g_renderer, 0x14, 0x14, 0x14, 0xff);
    SDL_RenderClear(g_renderer);

    /* The un-rotated body rect is centred in the window; turned by g_rotation
     * about its own centre it fills the window at every angle. */
    SDL_Rect dst{(w - body_w) / 2, (h - body_h) / 2, body_w, body_h};
    SDL_RenderCopyEx(g_renderer, g_device_texture, nullptr, &dst,
                     static_cast<double>(g_rotation), nullptr, SDL_FLIP_NONE);

    SDL_RenderPresent(g_renderer);
}

int device_width() { return DEVICE_W; }
int device_height() { return DEVICE_H; }
int scale() { return g_scale; }

void set_deck_visible(bool on) {
    if (g_deck_visible == on) return;
    g_deck_visible = on;
    /* The body extent changed, so the floating window must follow; while
     * fullscreen the compositor owns the size and render() recentres instead.
     * The off-screen textures are rebuilt lazily because their size changed. */
    if (!g_fullscreen && g_window != nullptr) {
        int w = 0, h = 0;
        current_window_size(w, h);
        SDL_SetWindowSize(g_window, w, h);
    }
    g_needs_redraw = true;
}

bool deck_visible() { return g_deck_visible; }

void set_rotation(int degrees) {
    int d = ((degrees % 360) + 360) % 360;
    d = (d / 90) * 90;
    if (d == g_rotation) return;
    g_rotation = d;
    layout_controls();
    if (!g_fullscreen && g_window != nullptr) {
        int w = 0, h = 0;
        current_window_size(w, h);
        SDL_SetWindowSize(g_window, w, h);
    }
    g_needs_redraw = true;
}

int rotation() { return g_rotation; }

bool consume_redraw() {
    const bool needed = g_needs_redraw;
    g_needs_redraw = false;
    return needed;
}

void inject_key_event(int sdl_keycode, bool down) {
    SDL_Event ev{};
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type = ev.type;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat = 0;
    ev.key.keysym.sym = static_cast<SDL_Keycode>(sdl_keycode);
    SDL_PushEvent(&ev);
}

void inject_mouse_click(int window_x, int window_y) {
    SDL_Event down{};
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.type = SDL_MOUSEBUTTONDOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.state = SDL_PRESSED;
    down.button.clicks = 1;
    down.button.x = window_x;
    down.button.y = window_y;
    SDL_PushEvent(&down);

    SDL_Event up{};
    up.type = SDL_MOUSEBUTTONUP;
    up.button.type = SDL_MOUSEBUTTONUP;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.state = SDL_RELEASED;
    up.button.clicks = 1;
    up.button.x = window_x;
    up.button.y = window_y;
    SDL_PushEvent(&up);
}

void set_launcher_mode(bool on) {
    g_launcher_mode = on;
    if (on) {
        g_launcher_events.clear();
        g_launcher_read = 0;
    }
}

bool launcher_poll(LauncherEvent& out) {
    if (g_launcher_read >= g_launcher_events.size()) {
        /* Fully drained: drop the consumed prefix so the queue does not grow
         * without bound over a long setup session. */
        if (g_launcher_read > 0) {
            g_launcher_events.clear();
            g_launcher_read = 0;
        }
        return false;
    }
    out = g_launcher_events[g_launcher_read++];
    return true;
}

}  // namespace lcd_sdl

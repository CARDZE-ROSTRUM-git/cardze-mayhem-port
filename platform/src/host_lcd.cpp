/*
 * LCD surface backing store shared between the emulated PortaPack IO bus and
 * the SDL window.
 *
 * The upstream ILI9341 driver issues CASET/PASET/RAMWR commands and then
 * streams 16-bit pixels through portapack::IO. Here those commands are decoded
 * into an RGB565 framebuffer; lcd_sdl.cpp presents it.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#include "host_lcd.hpp"
#include "host_portapack_io.hpp"

#include <atomic>
#include <cstring>

namespace portapack {
DeviceType device_type = DEV_PORTAPACK;
IO io;
}  // namespace portapack

namespace host_lcd {

namespace {

/*
 * The panel's native memory is addressed by the driver with MADCTL MY|MX set
 * (both axes reversed), so driver memory (x, y) lands at display
 * (WIDTH-1-x, HEIGHT-1-y). We keep the framebuffer in *display* orientation and
 * flip coordinates on write, which is exactly what the hardware does.
 */
uint16_t frame[WIDTH * HEIGHT];
std::atomic<bool> dirty{true};

/* Backlight / screen-on state. Defaults to on (the boot path turns it on). */
std::atomic<bool> backlight{true};

/* Current address window (inclusive), in driver memory coordinates. */
int win_x0 = 0;
int win_y0 = 0;
int win_x1 = WIDTH - 1;
int win_y1 = HEIGHT - 1;

/* Write cursor and wrap behaviour, mirroring the ILI9341's own pointer. */
int cur_x = 0;
int cur_y = 0;

/* Vertical scroll state, as programmed by VSCRDEF (0x33) / VSCRSADD (0x37).
 * Defaults are the power-on identity: whole-frame region starting at line 0. */
int scroll_top_y = 0;
int scroll_size = HEIGHT;
int scroll_vsp = 0;

inline void put_pixel_flipped(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
    frame[y * WIDTH + x] = color;
}

}  // namespace

uint16_t* pixels() {
    return frame;
}

void set_backlight(bool on) {
    backlight.store(on, std::memory_order_relaxed);
}

bool backlight_on() {
    return backlight.load(std::memory_order_relaxed);
}

size_t read_pixels_rgb888(int x, int y, int w, int h, uint8_t* out, size_t max_bytes) {
    if (out == nullptr || w <= 0 || h <= 0) return 0;

    size_t written = 0;
    for (int row = 0; row < h; row++) {
        const int dy = y + row;
        if (dy < 0 || dy >= HEIGHT) continue;

        /* Scan out through the same circular mapping the presentation uses, so
         * a screenshot of a scrolled region matches what is on the panel. */
        const uint16_t* srow = frame + static_cast<size_t>(display_row_source(dy)) * WIDTH;
        for (int col = 0; col < w; col++) {
            const int dx = x + col;
            if (dx < 0 || dx >= WIDTH) continue;
            if (written + 3 > max_bytes) return written;

            const uint16_t p = srow[dx];
            const uint8_t r5 = (p >> 11) & 0x1f;
            const uint8_t g6 = (p >> 5) & 0x3f;
            const uint8_t b5 = p & 0x1f;
            /* Expand each channel to 8 bits the way the display LUT does. */
            out[written++] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            out[written++] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            out[written++] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        }
    }
    return written;
}

void set_window(int x_start, int y_start, int x_end, int y_end) {
    win_x0 = x_start;
    win_y0 = y_start;
    win_x1 = x_end;
    win_y1 = y_end;
    cur_x = x_start;
    cur_y = y_start;
}

void set_write_cursor(int x, int y) {
    cur_x = x;
    cur_y = y;
}

void write_streaming_pixel(uint16_t color) {
    put_pixel_flipped(cur_x, cur_y, color);

    /* The controller advances the column pointer within the window and wraps
     * to the next row (and, if WEMODE is set, back to the row start). */
    if (cur_x < win_x1) {
        cur_x++;
    } else {
        cur_x = win_x0;
        if (cur_y < win_y1) {
            cur_y++;
        } else {
            cur_y = win_y0;
        }
    }

    /* Any pixel write dirties the surface; the SDL thread uploads it on its
     * next frame. Cheap enough (one relaxed store per pixel) and keeps the
     * presentation path independent of who drew. */
    dirty.store(true, std::memory_order_relaxed);
}

void clear(uint16_t color) {
    for (size_t i = 0; i < static_cast<size_t>(WIDTH) * HEIGHT; i++) {
        frame[i] = color;
    }
    dirty = true;
}

void mark_dirty() {
    dirty = true;
}

bool consume_dirty() {
    return dirty.exchange(false);
}

void set_scroll_area(int top_y, int size, int bottom_y) {
    (void)bottom_y;
    if (size <= 0) size = HEIGHT;
    scroll_top_y = top_y;
    scroll_size = size;
}

void set_scroll_position(int position) {
    /* VSCRSADD carries an absolute frame-memory line address (the driver sends
     * top_area + current_position); keep it as-is and fold it into the region
     * relative offset at scan-out time. */
    scroll_vsp = position;
}

int display_row_source(int display_y) {
    if (scroll_size <= 0) return display_y;
    if (display_y < scroll_top_y || display_y >= scroll_top_y + scroll_size) {
        return display_y;  /* outside the scrolling region: scanned 1:1 */
    }
    const int rel = (((scroll_vsp - scroll_top_y) % scroll_size) + scroll_size) % scroll_size;
    const int d = display_y - scroll_top_y;
    return scroll_top_y + ((rel + d) % scroll_size);
}

}  // namespace host_lcd

namespace portapack {

void IO::init() {}

void IO::lcd_backlight(const bool value) {
    host_lcd::set_backlight(value);
}

void IO::lcd_reset_state(const bool active) {
    (void)active;
}

void IO::audio_reset_state(const bool active) {
    (void)active;
}

void IO::reference_oscillator(const bool enable) {
    (void)enable;
}

uint32_t IO::io_update(const TouchPinsConfig write_value) {
    (void)write_value;
    return 0;
}

void IO::lcd_read_bytes(uint8_t* byte, size_t byte_count) {
    if (byte == nullptr || byte_count == 0) return;

    /* The upstream camera/screenshot path programmes a read window with
     * CASET/PASET then RAMRD and pulls packed ui::ColorRGB888 (r,g,b) bytes.
     * Serve them from the emulated framebuffer so the saved PNG is the real
     * screen, not zeros. The window is the inclusive CASET/PASET rectangle. */
    const int w = caset_x1 - caset_x0 + 1;
    const int h = caset_y1 - caset_y0 + 1;
    const size_t written = host_lcd::read_pixels_rgb888(
        caset_x0, caset_y0, w, h, byte, byte_count);

    /* Zero any tail the caller expected but the window did not cover (a driver
     * bug, or a window partially off-panel), so no stale bytes leak through. */
    if (written < byte_count) {
        std::memset(byte + written, 0, byte_count - written);
    }
}

/*
 * Minimal command interpreter. Only the commands the upstream driver relies on
 * for geometry and pixel streaming are decoded; everything else is ignored,
 * because it configures analog panel parameters we do not emulate.
 */
void IO::lcd_command(const uint32_t value) {
    switch (value & 0xff) {
        case 0x2A:  /* CASET: column address set, 4 data bytes */
        case 0x2B:  /* PASET: page address set, 4 data bytes */
        case 0x2C:  /* RAMWR: memory write */
        case 0x2E:  /* RAMRD: memory read (ignored) */
        case 0x33:  /* VSCRDEF: vertical scrolling definition, 6 data bytes */
        case 0x37:  /* VSCRSADD: vertical scrolling start address, 2 data bytes */
            pending_command = static_cast<uint8_t>(value & 0xff);
            command_args = 0;
            break;

        default:
            pending_command = 0;
            command_args = 0;
            break;
    }
}

void IO::lcd_write_data(const uint32_t value) {
    if (pending_command == 0) return;

    switch (pending_command) {
        case 0x2A:
        case 0x2B: {
            /* Collect four parameters (low byte each), then apply the window. */
            if (command_args < 4) {
                args[command_args++] = static_cast<uint8_t>(value & 0xff);
            }
            if (command_args == 4) {
                const int start = (args[0] << 8) | args[1];
                const int end = (args[2] << 8) | args[3];
                if (pending_command == 0x2A) {
                    caset_x0 = start;
                    caset_x1 = end;
                } else {
                    caset_y0 = start;
                    caset_y1 = end;
                }
                host_lcd::set_window(caset_x0, caset_y0, caset_x1, caset_y1);
                pending_command = 0;
            }
            break;
        }

        case 0x2C:
            /* Pixel streaming: lcd_write_pixel() passes the whole 16-bit
             * value here (the real bus would split it into two writes). */
            host_lcd::write_streaming_pixel(static_cast<uint16_t>(value & 0xffff));
            break;

        case 0x33: {
            /* VSCRDEF: TFA(2), VSA(2), BFA(2), big-endian byte pairs. */
            if (command_args < 6) {
                args[command_args++] = static_cast<uint8_t>(value & 0xff);
            }
            if (command_args == 6) {
                const int tfa = (args[0] << 8) | args[1];
                const int vsa = (args[2] << 8) | args[3];
                const int bfa = (args[4] << 8) | args[5];
                host_lcd::set_scroll_area(tfa, vsa, bfa);
                pending_command = 0;
            }
            break;
        }

        case 0x37: {
            /* VSCRSADD: vertical scrolling start address, one byte pair. */
            if (command_args < 2) {
                args[command_args++] = static_cast<uint8_t>(value & 0xff);
            }
            if (command_args == 2) {
                host_lcd::set_scroll_position((args[0] << 8) | args[1]);
                pending_command = 0;
            }
            break;
        }

        case 0x2E:
            break;

        default:
            break;
    }
}

}  // namespace portapack

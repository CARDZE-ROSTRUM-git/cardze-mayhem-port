/*
 * Host LCD surface shared between the emulated PortaPack IO bus and the SDL
 * window. The upstream ILI9341 driver draws through portapack::IO, which is
 * redirected here into an RGB565 buffer; lcd_sdl.cpp blits that buffer to the
 * screen at the display refresh rate.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_LCD_HPP__
#define __HOST_LCD_HPP__

#include <cstdint>
#include <cstddef>

namespace host_lcd {

/* Native panel geometry (PortaPack H2 / ILI9341 in portrait). */
constexpr int WIDTH = 240;
constexpr int HEIGHT = 320;

/* Backing store, row-major, 16 bits per pixel (rrrrrGGGGGGbbbbb). Memory
 * (0,0) is the top-left of the displayed image for the portrait orientation,
 * which is what the upstream driver configures via MADCTL MY|MX. */
uint16_t* pixels();

/* Address window used by CASET/PASET/RAMWR emulation. Coordinates are inclusive. */
void set_window(int x_start, int y_start, int x_end, int y_end);
void set_write_cursor(int x, int y);
void write_streaming_pixel(uint16_t color);

/* ILI9341 vertical-scroll emulation (VSCRDEF 0x33 / VSCRSADD 0x37).
 *
 * The panel does not move pixels when it scrolls: it scans a circular region
 * out of the frame memory starting from the scroll pointer. On the device the
 * waterfall relies on exactly that - each frame shifts the pointer by one line
 * and draws a single new row into the slot that just scrolled in - so a host
 * that merely writes the row in place never appears to move. Recording the
 * definition/pointer here lets the presentation step reproduce the scan-out.
 *
 * Scan-out (the region [top_y, top_y + size) of frame memory is circular):
 *   display row (top_y + d)  <-  frame row (top_y + ((position + d) % size))
 * Rows outside the region scan 1:1. With position 0 and a full-height region
 * this is the identity, so unscrolled drawing is unaffected. */
void set_scroll_area(int top_y, int size, int bottom_y);
void set_scroll_position(int position);

/* Frame-memory row that the panel scans out at the given display row. */
int display_row_source(int display_y);

/* Backlight (and therefore screen on/off) state. The upstream backlight driver
 * toggles it through portapack::IO::lcd_backlight(); the SDL layer reads it to
 * blank the panel when the firmware puts the display to sleep, and the input
 * layer reads it to know that any key/touch/wheel should wake the screen. */
void set_backlight(bool on);
bool backlight_on();

/* Copies the displayed pixels of the rectangle [x, x+w) x [y, y+h) as packed
 * RGB888 (r, g, b per pixel, the layout of ui::ColorRGB888) into `out`, walking
 * the same scan-out the presentation uses so a screenshot matches the screen
 * (including a scrolled waterfall). Writes at most `max_bytes` bytes and returns
 * the number written. Used by the host IO::lcd_read_bytes() so the Camera button
 * saves a real image from the framebuffer instead of zeros. */
size_t read_pixels_rgb888(int x, int y, int w, int h, uint8_t* out, size_t max_bytes);

/* Fills the whole surface with one color (used by the boot splash path). */
void clear(uint16_t color);

/* Marks the surface dirty; the SDL thread presents it on the next frame. */
void mark_dirty();

/* Returns true once per dirty period; used by the SDL presentation loop. */
bool consume_dirty();

}  // namespace host_lcd

#endif /* __HOST_LCD_HPP__ */

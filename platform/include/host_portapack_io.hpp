/*
 * Host replacement for firmware/common/portapack_io.hpp.
 *
 * The real header drives the LCD data bus bit-by-bit through GPIO, which is the
 * CPLD's parallel interface to the panel. On the desktop there is no CPLD, so
 * this header keeps the exact public interface of portapack::IO but implements
 * only what the upstream LCD driver calls, writing into an RGB565 framebuffer.
 *
 * Why a replacement header instead of a shadow include path: quoted includes
 * ("portapack_io.hpp") resolve to the including file's own directory first, so
 * an -I shadow would not be picked up by common/lcd_ili9341.cpp. Instead the
 * include guard __PORTAPACK_IO_H__ is pre-defined on the command line and this
 * header is force-included, exactly like lpc43xx_cpp.hpp.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_PORTAPACK_IO_HPP__
#define __HOST_PORTAPACK_IO_HPP__

#include <cstdint>
#include <cstddef>
#include <initializer_list>

#include "ui.hpp"
#include "host_lcd.hpp"

namespace portapack {

enum DeviceType {
    DEV_PORTAPACK,
    DEV_PORTARF,
};

extern DeviceType device_type;

class IO {
   public:
    enum class TouchPinsConfig : uint8_t {
        XN_BIT = (1 << 0),
        XP_BIT = (1 << 1),
        YN_BIT = (1 << 2),
        YP_BIT = (1 << 3),

        XN_OE = (1 << 4),
        XP_OE = (1 << 5),
        YN_OE = (1 << 6),
        YP_OE = (1 << 7),

        XN_IN = XN_BIT,
        XN_OUT_1 = XN_OE | XN_BIT,
        XN_OUT_0 = XN_OE,

        XP_IN = XP_BIT,
        XP_OUT_1 = XP_OE | XP_BIT,
        XP_OUT_0 = XP_OE,

        YN_IN = YN_BIT,
        YN_OUT_1 = YN_OE | YN_BIT,
        YN_OUT_0 = YN_OE,

        YP_IN = YP_BIT,
        YP_OUT_1 = YP_OE | YP_BIT,
        YP_OUT_0 = YP_OE,

        Float = XP_IN | XN_IN | YP_IN | YN_IN,

        WaitTouch = XP_OUT_0 | XN_OUT_0 | YP_IN | YN_IN,
        SensePressure = XP_IN | XN_OUT_0 | YP_OUT_1 | YN_IN,
        SenseX = XP_OUT_1 | XN_OUT_0 | YP_IN | YN_IN,
        SenseY = XP_IN | XN_IN | YP_OUT_1 | YN_OUT_0,
    };

    IO() = default;

    void init();

    void lcd_backlight(const bool value);
    void lcd_reset_state(const bool active);
    void audio_reset_state(const bool active);
    void reference_oscillator(const bool enable);

    void lcd_data_write_command_and_data(
        const uint_fast8_t command,
        const uint8_t* data,
        const size_t data_count) {
        lcd_command(command);
        for (size_t i = 0; i < data_count; i++) {
            lcd_write_data(data[i]);
        }
    }

    void lcd_data_write_command_and_data(
        const uint_fast8_t command,
        const std::initializer_list<uint8_t>& data) {
        lcd_command(command);
        for (const auto d : data) {
            lcd_write_data(d);
        }
    }

    uint32_t lcd_read_data_raw() {
        return 0;
    }

    void lcd_data_read_command_and_data(
        const uint_fast8_t command,
        uint16_t* const data,
        const size_t data_count) {
        (void)command;
        for (size_t i = 0; i < data_count; i++) data[i] = 0;
    }

    void lcd_write_word(const uint32_t w) {
        lcd_write_data(w);
    }

    void lcd_write_words(const uint16_t* const w, size_t n) {
        for (size_t i = 0; i < n; i++) lcd_write_data(w[i]);
    }

    void lcd_write_pixel(ui::Color pixel) {
        lcd_write_data(pixel.v);
    }

    uint32_t lcd_read_word() {
        return 0;
    }

    void lcd_write_pixels(ui::Color pixel, size_t n) {
        while (n--) lcd_write_data(pixel.v);
    }

    void lcd_write_pixels_unrolled8(ui::Color pixel, size_t n) {
        auto v = pixel.v;
        n >>= 3;
        while (n--) {
            for (int i = 0; i < 8; i++) lcd_write_data(v);
        }
    }

    void lcd_write_pixels(const ui::Color* const pixels, size_t n) {
        for (size_t i = 0; i < n; i++) lcd_write_pixel(pixels[i]);
    }

    void lcd_read_bytes(uint8_t* byte, size_t byte_count);

    uint32_t io_read() {
        return 0;
    }

    uint32_t io_update(const TouchPinsConfig write_value);

    uint32_t lcd_te() {
        return 0;
    }

   private:
    void lcd_command(const uint32_t value);
    void lcd_write_data(const uint32_t value);

    /* Command decoder state. RAMWR pixels are streamed as 16-bit values split
     * across two 8-bit bus writes, high byte first. */
    uint8_t pending_command{0};
    uint8_t command_args{0};
    uint8_t args[8]{0, 0, 0, 0, 0, 0, 0, 0};
    int caset_x0{0};
    int caset_x1{0};
    int caset_y0{0};
    int caset_y1{0};
};

extern IO io;

}  // namespace portapack

#endif /* __HOST_PORTAPACK_IO_HPP__ */

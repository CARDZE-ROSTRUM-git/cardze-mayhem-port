/*
 * Pre-boot setup window (launcher) for the desktop PortaPack. See
 * host_launcher.hpp for the rationale.
 *
 * Everything here runs before portapack::init(), so it cannot use the firmware
 * UI tree or the controls scan. Instead it draws directly into the same 240x320
 * RGB565 panel surface the firmware will later paint into (host_lcd), and reads
 * raw panel clicks / keys through the SDL layer's launcher mode. Text is drawn
 * with the firmware's own bitmap fonts, so no extra dependency is needed.
 *
 * This translation unit uses the real C++17 <filesystem> (like platform/src/
 * ff_stub.cpp and host_regs.cpp) and is therefore built with HOST_STD_FILESYSTEM
 * so the firmware's FatFs shim is skipped for it.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "host_launcher.hpp"

#include "host_lcd.hpp"
#include "host_settings.hpp"
#include "lcd_sdl.hpp"
#include "sdr_device.hpp"

/* hal.h (force-included) defines __I/__O/__IO as LPC43xx register qualifiers;
 * SDL's <immintrin.h> reuses those tokens. Drop them before SDL is seen. */
#undef __I
#undef __O
#undef __IO

#include <SDL.h>

#include "ui.hpp"
#include "ui_text.hpp"
#include "ui_font_fixed_5x8.hpp"
#include "ui_font_fixed_8x16.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace host_launcher {

namespace {

namespace fs = std::filesystem;

/* ---------------------------------------------------------------------- */
/* Small RGB565 drawing helpers over the panel surface.                     */
/* ---------------------------------------------------------------------- */

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3));
}

constexpr uint16_t COL_BG = rgb(18, 18, 20);
constexpr uint16_t COL_HEADER = rgb(30, 60, 110);
constexpr uint16_t COL_SECTION = rgb(120, 190, 255);
constexpr uint16_t COL_ROW = rgb(40, 40, 44);
constexpr uint16_t COL_ROW_FOCUS = rgb(70, 90, 130);
constexpr uint16_t COL_TEXT = rgb(230, 230, 230);
constexpr uint16_t COL_TEXT_DIM = rgb(150, 150, 155);
constexpr uint16_t COL_ACCENT = rgb(90, 200, 120);
constexpr uint16_t COL_QUIT = rgb(200, 90, 90);

void fill_rect(int x, int y, int w, int h, uint16_t color) {
    uint16_t* px = host_lcd::pixels();
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= host_lcd::HEIGHT) continue;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= host_lcd::WIDTH) continue;
            px[yy * host_lcd::WIDTH + xx] = color;
        }
    }
}

void put_pixel(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= host_lcd::WIDTH || y >= host_lcd::HEIGHT) return;
    host_lcd::pixels()[y * host_lcd::WIDTH + x] = color;
}

/* Draws one line of text with a firmware bitmap font. Returns the advance in
 * pixels (glyph widths), matching ui::Painter. */
int draw_text(const ui::Font& font, int x, int y, const std::string& text,
              uint16_t fg, uint16_t bg) {
    int cx = x;
    for (const char c : text) {
        const ui::Glyph glyph = font.glyph(c);
        const int gw = glyph.w();
        const int gh = glyph.h();
        const uint8_t* pixels = glyph.pixels();
        for (int i = 0; i < gw * gh; ++i) {
            const bool on = (pixels[i >> 3] & (1U << (i & 7))) != 0;
            put_pixel(cx + (i % gw), y + (i / gw), on ? fg : bg);
        }
        cx += gw;
    }
    return cx - x;
}

int text_width(const ui::Font& font, const std::string& text) {
    int w = 0;
    for (const char c : text) w += font.glyph(c).w();
    return w;
}

/* Truncates text with a trailing ".." so it fits within max_px pixels. */
std::string fit_text(const ui::Font& font, const std::string& text, int max_px) {
    if (text_width(font, text) <= max_px) return text;
    const int ell = text_width(font, "..");
    std::string out;
    int w = 0;
    for (const char c : text) {
        const int cw = font.glyph(c).w();
        if (w + cw + ell > max_px) break;
        out += c;
        w += cw;
    }
    return out + "..";
}

void draw_text_center(const ui::Font& font, int x, int y, int w, const std::string& text,
                      uint16_t fg, uint16_t bg) {
    const int tw = text_width(font, text);
    draw_text(font, x + (w - tw) / 2, y, text, fg, bg);
}

/* Hard-wraps text at max_chars characters, preserving every character (so an
 * absolute path keeps its leading '/'). The 5x8 font is fixed-width. */
std::vector<std::string> wrap(const std::string& text, int max_chars) {
    std::vector<std::string> lines;
    if (max_chars < 1) max_chars = 1;
    for (size_t i = 0; i < text.size(); i += max_chars) {
        lines.push_back(text.substr(i, max_chars));
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

/* ---------------------------------------------------------------------- */
/* Layout (panel pixels, 240x320 portrait).                                 */
/* ---------------------------------------------------------------------- */

constexpr int kRowX = 6;
constexpr int kRowW = host_lcd::WIDTH - 12;

/* Main page. */
constexpr int kBrowseY = 86;
constexpr int kBrowseH = 22;
/* Device section header: a filled band with a large centered caption, so the
 * "SDR DEVICE" grouping reads as a heading instead of a dim 5x8 label that the
 * driver row below it used to overlap. */
constexpr int kDevHeadY = 112;
constexpr int kDevHeadH = 20;
constexpr int kDriverY = 136;
constexpr int kDriverH = 22;
constexpr int kDeviceY = 160;
constexpr int kDeviceH = 22;
constexpr int kControlsY = 194;
constexpr int kControlsH = 26;
constexpr int kBattY = 226;
constexpr int kBattH = 26;
constexpr int kBtnY = 276;
constexpr int kBtnH = 36;
constexpr int kStartX = 6;
constexpr int kStartW = 112;
constexpr int kQuitX = 122;
constexpr int kQuitW = 112;

/* Controls & Display page rows. */
constexpr int kCDRowH = 30;
constexpr int kCDTop = 40;

/* Key-bindings page: one row per remappable action. */
constexpr int kBindRowH = 18;
constexpr int kBindTop = 34;

struct Focus {
    enum Value { Browse = 0, Driver, Device, Controls, Battery, Start, Quit, Count };
};

bool point_in(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* Builds the list of devices the current driver can see. */
std::vector<sdr::DeviceInfo> devices_for(const std::string& driver,
                                         const std::vector<sdr::DeviceInfo>& all) {
    std::vector<sdr::DeviceInfo> out;
    for (const auto& d : all) {
        if (d.driver == driver) out.push_back(d);
    }
    return out;
}

/* ---------------------------------------------------------------------- */
/* Directory browser overlay.                                               */
/* ---------------------------------------------------------------------- */

/* Lets the user walk the filesystem and pick a folder. Returns the chosen path
 * in `out` and true on confirmation, false on cancel. */
bool browse_dialog(fs::path& current, int scale) {
    std::vector<fs::path> entries; /* first entry: the "[ use this folder ]" pseudo-item */
    int selected = 0;
    int scroll = 0;

    auto rescan = [&] {
        entries.clear();
        entries.push_back(current); /* pseudo-item sentinel for "use this folder" */
        entries.push_back(current / "..");

        std::error_code ec;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(current, ec)) {
            std::error_code ec2;
            if (e.is_directory(ec2)) dirs.push_back(e.path());
            if (ec) break;
        }
        std::sort(dirs.begin(), dirs.end());
        for (auto& d : dirs) {
            entries.push_back(d);
        }
        selected = 0;
        scroll = 0;
    };
    rescan();

    constexpr int kListTop = 40;
    constexpr int kListRowH = 14;
    constexpr int kVisible = (kBtnY - kListTop) / kListRowH;

    for (;;) {
        /* Drain input. */
        lcd_sdl::pump();
        bool canceled = false, confirmed = false, changed = false;
        lcd_sdl::LauncherEvent ev;
        while (lcd_sdl::launcher_poll(ev)) {
            if (ev.type == lcd_sdl::LauncherEvent::Type::Quit) {
                canceled = true;
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Key) {
                switch (ev.key) {
                    case SDLK_ESCAPE: canceled = true; break;
                    case SDLK_UP: selected = (selected + entries.size() - 1) % entries.size(); changed = true; break;
                    case SDLK_DOWN: selected = (selected + 1) % entries.size(); changed = true; break;
                    case SDLK_LEFT: {
                        fs::path parent = current.parent_path();
                        if (!parent.empty()) { current = parent; rescan(); changed = true; }
                        break;
                    }
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER: confirmed = true; break;
                    default: break;
                }
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Click) {
                if (ev.y >= kListTop && ev.y < kBtnY) {
                    const int idx = scroll + (ev.y - kListTop) / kListRowH;
                    if (idx >= 0 && idx < static_cast<int>(entries.size())) {
                        selected = idx;
                        confirmed = true;
                    }
                } else if (point_in(ev.x, ev.y, kStartX, kBtnY, kStartW, kBtnH)) {
                    confirmed = true;
                } else if (point_in(ev.x, ev.y, kQuitX, kBtnY, kQuitW, kBtnH)) {
                    canceled = true;
                }
            }
        }

        if (canceled) return false;

        if (confirmed) {
            if (selected == 0) {
                return true; /* "use this folder" */
            }
            if (selected == 1) {
                fs::path parent = current.parent_path();
                if (!parent.empty()) { current = parent; rescan(); changed = true; }
            } else {
                const fs::path& p = entries[selected];
                std::error_code ec;
                if (fs::is_directory(p, ec)) { current = p; rescan(); changed = true; }
            }
        }

        /* Keep the selection visible. */
        if (changed) {
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + kVisible) scroll = selected - kVisible + 1;
            if (scroll < 0) scroll = 0;
        }

        /* Draw. */
        fill_rect(0, 0, host_lcd::WIDTH, host_lcd::HEIGHT, COL_BG);
        fill_rect(0, 0, host_lcd::WIDTH, 40, COL_HEADER);
        draw_text_center(ui::font::fixed_8x16, 0, 4, host_lcd::WIDTH, "Browse folder", COL_TEXT, COL_HEADER);
        {
            /* Current directory, wrapped to two lines at 5x8 in the header. */
            auto lines = wrap(current.string(), 46);
            int y = 22;
            for (size_t i = 0; i < lines.size() && i < 2; ++i, y += 9) {
                draw_text(ui::font::fixed_5x8, 6, y, lines[i], COL_TEXT_DIM, COL_HEADER);
            }
        }

        for (int row = 0; row < kVisible; ++row) {
            const int idx = scroll + row;
            if (idx >= static_cast<int>(entries.size())) break;
            const int y = kListTop + row * kListRowH;
            const bool sel = (idx == selected);
            fill_rect(6, y, kRowW, kListRowH - 1, sel ? COL_ROW_FOCUS : COL_ROW);

            std::string label;
            if (idx == 0) {
                label = "[ Use this folder ]";
            } else if (idx == 1) {
                label = "..";
            } else {
                label = entries[idx].filename().string();
            }
            draw_text(ui::font::fixed_5x8, 10, y + 3,
                      fit_text(ui::font::fixed_5x8, label, kRowW - 10), sel ? COL_TEXT : COL_TEXT_DIM,
                      sel ? COL_ROW_FOCUS : COL_ROW);
        }

        fill_rect(kStartX, kBtnY, kStartW, kBtnH, COL_ACCENT);
        fill_rect(kQuitX, kBtnY, kQuitW, kBtnH, COL_QUIT);
        draw_text_center(ui::font::fixed_8x16, kStartX, kBtnY + 8, kStartW, "Select", rgb(10, 40, 15), COL_ACCENT);
        draw_text_center(ui::font::fixed_8x16, kQuitX, kBtnY + 8, kQuitW, "Cancel", rgb(40, 10, 10), COL_QUIT);

        lcd_sdl::render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        (void)scale;
    }
}

/* ---------------------------------------------------------------------- */
/* Key bindings are stored as physical scancodes (see host_settings), so     */
/* the two dialogs below can rebind them: Controls & Display offers the       */
/* mode/rotation/wheel options plus a way into the key editor.                */
/* ---------------------------------------------------------------------- */

bool key_bindings_dialog();

/* ---------------------------------------------------------------------- */
/* Key-bindings editor.                                                     */
/*                                                                          */
/* One row per remappable action. Selecting a row starts a capture: the       */
/* next physical key pressed becomes the binding (Escape cancels). Bindings   */
/* are physical scancodes, so they are independent of the keyboard layout.    */
/* ---------------------------------------------------------------------- */

bool key_bindings_dialog() {
    const int count = static_cast<int>(host_settings::Action::Count);
    int sel = 0;
    bool capturing = false;
    host_settings::Action capturing_action = host_settings::Action::NavUp;

    auto commit = [&] { host_settings::save(); };

    auto begin_capture = [&](int row) {
        capturing_action = static_cast<host_settings::Action>(row);
        capturing = true;
        lcd_sdl::begin_key_capture();
    };

    for (;;) {
        lcd_sdl::pump();

        /* A captured key (if any) becomes the new binding. */
        int scan = 0;
        if (lcd_sdl::poll_captured_scancode(scan)) {
            host_settings::set_scancode(capturing_action, scan);
            capturing = false;
            commit();
        } else if (capturing && !lcd_sdl::key_capture_active()) {
            /* Capture was cancelled (Escape). */
            capturing = false;
        }

        bool leave = false;
        lcd_sdl::LauncherEvent ev;
        while (lcd_sdl::launcher_poll(ev)) {
            if (ev.type == lcd_sdl::LauncherEvent::Type::Quit) {
                leave = true;
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Key) {
                if (capturing) continue; /* keys are being captured, not navigated */
                switch (ev.key) {
                    case SDLK_ESCAPE: leave = true; break;
                    case SDLK_UP: sel = (sel + count - 1) % count; break;
                    case SDLK_DOWN: sel = (sel + 1) % count; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER: begin_capture(sel); break;
                    default: break;
                }
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Click) {
                if (ev.y >= kBindTop && ev.y < kBindTop + count * kBindRowH) {
                    sel = (ev.y - kBindTop) / kBindRowH;
                    if (sel >= 0 && sel < count) begin_capture(sel);
                } else if (point_in(ev.x, ev.y, kQuitX, kBtnY, kQuitW, kBtnH)) {
                    leave = true;
                }
            }
        }
        if (leave) return true;

        fill_rect(0, 0, host_lcd::WIDTH, host_lcd::HEIGHT, COL_BG);
        fill_rect(0, 0, host_lcd::WIDTH, 26, COL_HEADER);
        draw_text_center(ui::font::fixed_8x16, 0, 5, host_lcd::WIDTH, "KEY BINDINGS", COL_TEXT, COL_HEADER);

        for (int r = 0; r < count; ++r) {
            const int y = kBindTop + r * kBindRowH;
            const bool s = (r == sel);
            const auto a = static_cast<host_settings::Action>(r);
            fill_rect(kRowX, y, kRowW, kBindRowH - 1, s ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + 4, y + 3, host_settings::action_label(a), COL_TEXT,
                      s ? COL_ROW_FOCUS : COL_ROW);
            std::string keylabel;
            if (capturing && capturing_action == a) {
                keylabel = "press key..";
            } else {
                keylabel = lcd_sdl::scancode_name(host_settings::scancode(a));
            }
            const int kw = text_width(ui::font::fixed_5x8, keylabel);
            draw_text(ui::font::fixed_5x8, kRowX + kRowW - 6 - kw, y + 3, keylabel,
                      (capturing && capturing_action == a) ? COL_ACCENT : COL_TEXT,
                      s ? COL_ROW_FOCUS : COL_ROW);
        }

        fill_rect(kQuitX, kBtnY, kQuitW, kBtnH, COL_QUIT);
        draw_text_center(ui::font::fixed_8x16, kQuitX, kBtnY + 9, kQuitW, "BACK",
                         rgb(40, 10, 10), COL_QUIT);

        lcd_sdl::render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

bool controls_dialog() {
    int sel = 0; /* 0 = input mode, 1 = rotation, 2 = wheel, 3 = bindings, 4 = back */
    constexpr int kRows = 5;
    bool changed = false;

    /* Applies the input-mode choice immediately so the window previews it. */
    auto toggle_input_mode = [&] {
        host_settings::set_touch_only(!host_settings::touch_only());
        lcd_sdl::set_deck_visible(!host_settings::touch_only());
        changed = true;
    };
    auto cycle_rotation = [&](int dir) {
        const int idx = (host_settings::rotation() / 90 + dir + 4) % 4;
        host_settings::set_rotation(idx * 90);
        lcd_sdl::set_rotation(host_settings::rotation());
        changed = true;
    };
    auto toggle_wheel = [&] {
        host_settings::set_wheel_inverted(!host_settings::wheel_inverted());
        changed = true;
    };

    for (;;) {
        lcd_sdl::pump();
        bool leave = false;
        lcd_sdl::LauncherEvent ev;
        while (lcd_sdl::launcher_poll(ev)) {
            if (ev.type == lcd_sdl::LauncherEvent::Type::Quit) {
                leave = true;
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Key) {
                switch (ev.key) {
                    case SDLK_ESCAPE: leave = true; break;
                    case SDLK_UP: sel = (sel + kRows - 1) % kRows; break;
                    case SDLK_DOWN: sel = (sel + 1) % kRows; break;
                    case SDLK_LEFT:
                    case SDLK_RIGHT: {
                        const int dir = (ev.key == SDLK_RIGHT) ? +1 : -1;
                        if (sel == 0) {
                            toggle_input_mode();
                        } else if (sel == 1) {
                            cycle_rotation(dir);
                        } else if (sel == 2) {
                            toggle_wheel();
                        }
                        break;
                    }
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        if (sel == 0) {
                            toggle_input_mode();
                        } else if (sel == 1) {
                            cycle_rotation(+1);
                        } else if (sel == 2) {
                            toggle_wheel();
                        } else if (sel == 3) {
                            key_bindings_dialog();
                            changed = true;
                        } else {
                            leave = true;
                        }
                        break;
                    default: break;
                }
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Click) {
                const int row = (ev.y - kCDTop) / kCDRowH;
                if (ev.y >= kCDTop && row >= 0 && row < kRows) {
                    sel = row;
                    if (row == 0) {
                        toggle_input_mode();
                    } else if (row == 1) {
                        cycle_rotation(+1);
                    } else if (row == 2) {
                        toggle_wheel();
                    } else if (row == 3) {
                        key_bindings_dialog();
                        changed = true;
                    } else {
                        leave = true;
                    }
                } else if (point_in(ev.x, ev.y, kQuitX, kBtnY, kQuitW, kBtnH)) {
                    leave = true;
                }
            }
        }
        if (leave) return changed;

        fill_rect(0, 0, host_lcd::WIDTH, host_lcd::HEIGHT, COL_BG);
        fill_rect(0, 0, host_lcd::WIDTH, 26, COL_HEADER);
        draw_text_center(ui::font::fixed_8x16, 0, 5, host_lcd::WIDTH, "CONTROLS & DISPLAY", COL_TEXT, COL_HEADER);

        auto row_rect = [](int r) { return std::array<int, 4>{kRowX, kCDTop + r * kCDRowH, kRowW, kCDRowH - 2}; };

        const char* rot_names[4] = {"Portrait", "Landscape", "Portrait flipped", "Landscape flipped"};

        struct RowText { std::string title; std::string sub; };
        RowText rows[kRows];
        rows[0] = {"Input mode", host_settings::touch_only() ? "Touchscreen only" : "Touchscreen + buttons"};
        rows[1] = {"Window rotation", std::string("Rotation: ") + rot_names[(host_settings::rotation() / 90) % 4]};
        rows[2] = {"Mouse wheel", host_settings::wheel_inverted() ? "Down = clockwise" : "Up = clockwise"};
        rows[3] = {"Key bindings", "Remap action keys"};
        rows[4] = {"Back", "Return to setup"};

        for (int r = 0; r < kRows; ++r) {
            const bool s = (r == sel);
            const auto rr = row_rect(r);
            fill_rect(rr[0], rr[1], rr[2], rr[3], s ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + 4, rr[1] + 4, rows[r].title, COL_TEXT,
                      s ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + 4, rr[1] + 14, rows[r].sub, COL_TEXT_DIM,
                      s ? COL_ROW_FOCUS : COL_ROW);
            if (r <= 2) {
                draw_text(ui::font::fixed_5x8, kRowX + kRowW - 14, rr[1] + 4, "<>", COL_TEXT_DIM,
                          s ? COL_ROW_FOCUS : COL_ROW);
            }
        }

        {
            const bool s = (sel == 4);
            fill_rect(kQuitX, kBtnY, kQuitW, kBtnH, COL_QUIT);
            draw_text_center(ui::font::fixed_8x16, kQuitX, kBtnY + 9, kQuitW, "BACK",
                             rgb(40, 10, 10), COL_QUIT);
            if (s) {
                fill_rect(kQuitX, kBtnY, kQuitW, 2, COL_TEXT);
                fill_rect(kQuitX, kBtnY + kBtnH - 2, kQuitW, 2, COL_TEXT);
            }
        }

        lcd_sdl::render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

}  // namespace

/* Numeric entry for the battery capacity. Returns the new value in mAh, 0 for
 * "auto" (read from the device), or -1 when cancelled. */
int battery_input_dialog(int current) {
    std::string buf = (current > 0 && current <= 20000) ? std::to_string(current) : "";
    bool done = false, cancel = false;

    /* On-screen step buttons for touch/mouse. A tap adds +/-10; holding for a
     * moment repeats with an accelerating interval. Rects are panel pixels.
     * Side margins match the 22 px gap above the row so the padding is even. */
    const int kMinusX = 22, kMinusY = 158, kMinusW = 88, kMinusH = 56;
    const int kPlusX = 130, kPlusY = 158, kPlusW = 88, kPlusH = 56;

    int hold_dir = 0; /* -1 / +1 while a button is held */
    std::chrono::steady_clock::time_point hold_start;
    std::chrono::steady_clock::time_point next_repeat;

    auto current_value = [&]() -> int {
        return buf.empty() ? 0 : std::atoi(buf.c_str());
    };
    auto set_value = [&](int v) {
        if (v < 0) v = 0;
        if (v > 20000) v = 20000;
        buf = (v == 0) ? std::string() : std::to_string(v);
    };
    auto step = [&](int dir) {
        int v = current_value();
        /* From "auto" a plus tap starts at a sane base, not at 10 mAh. */
        if (v == 0 && dir > 0) v = 1000;
        set_value(v + dir * 10);
    };

    while (!done) {
        lcd_sdl::pump();
        lcd_sdl::LauncherEvent ev;
        while (lcd_sdl::launcher_poll(ev)) {
            if (ev.type == lcd_sdl::LauncherEvent::Type::Quit) {
                cancel = true;
                done = true;
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Key) {
                const int k = ev.key;
                if (k == SDLK_ESCAPE) {
                    cancel = true;
                    done = true;
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    done = true;
                } else if (k == SDLK_BACKSPACE) {
                    if (!buf.empty()) buf.pop_back();
                } else if (k >= SDLK_0 && k <= SDLK_9) {
                    if (buf.size() < 5) buf.push_back(static_cast<char>('0' + (k - SDLK_0)));
                } else if (k >= SDLK_KP_0 && k <= SDLK_KP_9) {
                    if (buf.size() < 5) buf.push_back(static_cast<char>('0' + (k - SDLK_KP_0)));
                }
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Click) {
                if (point_in(ev.x, ev.y, kMinusX, kMinusY, kMinusW, kMinusH)) {
                    step(-1);
                    hold_dir = -1;
                    hold_start = std::chrono::steady_clock::now();
                    next_repeat = hold_start + std::chrono::milliseconds(400);
                } else if (point_in(ev.x, ev.y, kPlusX, kPlusY, kPlusW, kPlusH)) {
                    step(+1);
                    hold_dir = +1;
                    hold_start = std::chrono::steady_clock::now();
                    next_repeat = hold_start + std::chrono::milliseconds(400);
                } else if (point_in(ev.x, ev.y, kStartX, kBtnY, kStartW, kBtnH)) {
                    done = true;
                } else if (point_in(ev.x, ev.y, kQuitX, kBtnY, kQuitW, kBtnH)) {
                    cancel = true;
                    done = true;
                }
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Release) {
                hold_dir = 0;
            }
        }
        if (done) break;

        /* Hold-to-repeat: after the initial 400 ms the step interval shrinks
         * towards 20 ms, so a held button races through the range. */
        if (hold_dir != 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_repeat) {
                step(hold_dir);
                const auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - hold_start).count();
                int interval = 120 - static_cast<int>(std::min<long long>(100, (held_ms - 400) / 12));
                if (interval < 20) interval = 20;
                next_repeat = now + std::chrono::milliseconds(interval);
            }
        }

        fill_rect(0, 0, host_lcd::WIDTH, host_lcd::HEIGHT, COL_BG);
        fill_rect(0, 0, host_lcd::WIDTH, 26, COL_HEADER);
        draw_text_center(ui::font::fixed_8x16, 0, 5, host_lcd::WIDTH, "BATTERY CAPACITY", COL_TEXT, COL_HEADER);
        draw_text(ui::font::fixed_5x8, 12, 50, "Type mAh, or use -/+ (10 per tap).", COL_TEXT, COL_BG);
        draw_text(ui::font::fixed_5x8, 12, 62, "Hold -/+ to change fast.", COL_TEXT_DIM, COL_BG);

        fill_rect(12, 84, host_lcd::WIDTH - 24, 46, COL_ROW);
        const std::string shown = buf.empty() ? "auto" : (buf + " mAh");
        draw_text_center(ui::font::fixed_8x16, 12, 100, host_lcd::WIDTH - 24, shown, COL_TEXT, COL_ROW);
        draw_text_center(ui::font::fixed_5x8, 0, 136, host_lcd::WIDTH,
                         "Empty / 0 = auto (from device)", COL_TEXT_DIM, COL_BG);

        fill_rect(kMinusX, kMinusY, kMinusW, kMinusH, COL_ROW_FOCUS);
        draw_text_center(ui::font::fixed_8x16, kMinusX, kMinusY + 18, kMinusW, "-", COL_TEXT, COL_ROW_FOCUS);
        fill_rect(kPlusX, kPlusY, kPlusW, kPlusH, COL_ROW_FOCUS);
        draw_text_center(ui::font::fixed_8x16, kPlusX, kPlusY + 18, kPlusW, "+", COL_TEXT, COL_ROW_FOCUS);

        fill_rect(kStartX, kBtnY, kStartW, kBtnH, COL_ACCENT);
        draw_text_center(ui::font::fixed_8x16, kStartX, kBtnY + 8, kStartW, "OK",
                         rgb(10, 40, 15), COL_ACCENT);
        fill_rect(kQuitX, kBtnY, kQuitW, kBtnH, COL_QUIT);
        draw_text_center(ui::font::fixed_8x16, kQuitX, kBtnY + 8, kQuitW, "CANCEL",
                         rgb(40, 10, 10), COL_QUIT);

        lcd_sdl::render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (cancel) return -1;
    return current_value();
}

Result run(const std::string& default_sd, int scale) {
    Result result;
    result.start = false;
    result.sd_dir = default_sd;

    const std::vector<std::string> drivers = sdr::available_drivers();
    std::vector<sdr::DeviceInfo> all_devices = sdr::enumerate();

    int driver_idx = 0;
    int device_idx = 0;
    int focus = Focus::Browse;

    /* Window rotation, restored from the previous session's saved default and
     * applied immediately so the setup window is already turned. The full host
     * settings file (rotation, input mode, key bindings, wheel direction) is
     * loaded here as well; the Controls & Display page edits it in place. */
    host_settings::load();
    lcd_sdl::set_rotation(host_settings::rotation());

    auto sync_device_idx = [&] {
        device_idx = 0;
    };
    sync_device_idx();

    lcd_sdl::set_launcher_mode(true);

    bool done = false;
    while (!done) {
        /* Drain input. */
        lcd_sdl::pump();
        lcd_sdl::LauncherEvent ev;
        bool activate = false;
        while (lcd_sdl::launcher_poll(ev)) {
            if (ev.type == lcd_sdl::LauncherEvent::Type::Quit) {
                result.start = false;
                done = true;
                break;
            }
            if (ev.type == lcd_sdl::LauncherEvent::Type::Key) {
                switch (ev.key) {
                    case SDLK_UP: focus = (focus + Focus::Count - 1) % Focus::Count; break;
                    case SDLK_DOWN: focus = (focus + 1) % Focus::Count; break;
                    case SDLK_LEFT: {
                        if (focus == Focus::Driver && !drivers.empty()) {
                            driver_idx = (driver_idx + drivers.size() - 1) % drivers.size();
                            sync_device_idx();
                        } else if (focus == Focus::Device) {
                            auto devs = devices_for(drivers.empty() ? "" : drivers[driver_idx], all_devices);
                            if (!devs.empty()) device_idx = (device_idx + devs.size() - 1) % devs.size();
                        }
                        break;
                    }
                    case SDLK_RIGHT: {
                        if (focus == Focus::Driver && !drivers.empty()) {
                            driver_idx = (driver_idx + 1) % drivers.size();
                            sync_device_idx();
                        } else if (focus == Focus::Device) {
                            auto devs = devices_for(drivers.empty() ? "" : drivers[driver_idx], all_devices);
                            if (!devs.empty()) device_idx = (device_idx + 1) % devs.size();
                        }
                        break;
                    }
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER: activate = true; break;
                    case SDLK_ESCAPE: result.start = false; done = true; break;
                    default: break;
                }
            } else if (ev.type == lcd_sdl::LauncherEvent::Type::Click) {
                if (point_in(ev.x, ev.y, kRowX, kBrowseY, kRowW, kBrowseH)) {
                    focus = Focus::Browse;
                    activate = true;
                } else if (point_in(ev.x, ev.y, kRowX, kDriverY, kRowW, kDriverH)) {
                    focus = Focus::Driver;
                    if (!drivers.empty()) {
                        driver_idx = (driver_idx + 1) % drivers.size();
                        sync_device_idx();
                    }
                } else if (point_in(ev.x, ev.y, kRowX, kDeviceY, kRowW, kDeviceH)) {
                    focus = Focus::Device;
                    auto devs = devices_for(drivers.empty() ? "" : drivers[driver_idx], all_devices);
                    if (!devs.empty()) device_idx = (device_idx + 1) % devs.size();
                } else if (point_in(ev.x, ev.y, kRowX, kControlsY, kRowW, kControlsH)) {
                    focus = Focus::Controls;
                    activate = true;
                } else if (point_in(ev.x, ev.y, kRowX, kBattY, kRowW, kBattH)) {
                    focus = Focus::Battery;
                    activate = true;
                } else if (point_in(ev.x, ev.y, kStartX, kBtnY, kStartW, kBtnH)) {
                    focus = Focus::Start;
                    activate = true;
                } else if (point_in(ev.x, ev.y, kQuitX, kBtnY, kQuitW, kBtnH)) {
                    focus = Focus::Quit;
                    activate = true;
                }
            }
        }
        if (done) break;

        if (activate) {
            switch (focus) {
                case Focus::Browse: {
                    fs::path p(result.sd_dir);
                    std::error_code ec;
                    if (!fs::is_directory(p, ec)) p = fs::current_path(ec);
                    if (browse_dialog(p, scale)) {
                        result.sd_dir = p.string();
                    }
                    break;
                }
                case Focus::Start:
                    result.start = true;
                    host_settings::save();
                    done = true;
                    break;
                case Focus::Controls:
                    controls_dialog();
                    break;
                case Focus::Battery: {
                    const int v = battery_input_dialog(host_settings::battery_mah());
                    if (v >= 0) host_settings::set_battery_mah(v);
                    break;
                }
                case Focus::Quit:
                    result.start = false;
                    done = true;
                    break;
                default:
                    break;
            }
        }
        if (done) break;

        /* Draw one frame. */
        fill_rect(0, 0, host_lcd::WIDTH, host_lcd::HEIGHT, COL_BG);

        /* Header. */
        fill_rect(0, 0, host_lcd::WIDTH, 26, COL_HEADER);
        draw_text_center(ui::font::fixed_8x16, 0, 5, host_lcd::WIDTH, "MAYHEM DESKTOP", COL_TEXT, COL_HEADER);

        /* SD card section. */
        draw_text(ui::font::fixed_5x8, kRowX, 34, "SD CARD FOLDER", COL_SECTION, COL_BG);
        fill_rect(kRowX, 44, kRowW, 38, COL_ROW);
        {
            auto lines = wrap(result.sd_dir, 45);
            int y = 48;
            for (size_t i = 0; i < lines.size() && i < 4; ++i, y += 9) {
                draw_text(ui::font::fixed_5x8, kRowX + 4, y, lines[i], COL_TEXT, COL_ROW);
            }
        }

        {
            const bool sel = (focus == Focus::Browse);
            fill_rect(kRowX, kBrowseY, kRowW, kBrowseH, sel ? COL_ROW_FOCUS : COL_ROW);
            draw_text_center(ui::font::fixed_8x16, kRowX, kBrowseY + 4, kRowW, "Browse...",
                             COL_TEXT, sel ? COL_ROW_FOCUS : COL_ROW);
        }

        /* Device section: a full-width accent band with a large centered
         * caption, so the heading is unmistakable. */
        fill_rect(kRowX, kDevHeadY, kRowW, kDevHeadH, COL_SECTION);
        draw_text_center(ui::font::fixed_8x16, kRowX, kDevHeadY + 2, kRowW,
                         "SDR DEVICE", rgb(10, 25, 45), COL_SECTION);

        {
            const bool sel = (focus == Focus::Driver);
            fill_rect(kRowX, kDriverY, kRowW, kDriverH, sel ? COL_ROW_FOCUS : COL_ROW);
            const std::string name = drivers.empty() ? "(no driver)" : drivers[driver_idx];
            const std::string label = fit_text(ui::font::fixed_5x8, "Driver: " + name, kRowW - 8);
            draw_text(ui::font::fixed_5x8, kRowX + 4, kDriverY + 4, label, COL_TEXT,
                      sel ? COL_ROW_FOCUS : COL_ROW);
            if (drivers.size() > 1) {
                draw_text(ui::font::fixed_5x8, kRowX + kRowW - 12, kDriverY + 10, ">", COL_TEXT_DIM,
                          sel ? COL_ROW_FOCUS : COL_ROW);
            }
        }

        {
            const bool sel = (focus == Focus::Device);
            fill_rect(kRowX, kDeviceY, kRowW, kDeviceH, sel ? COL_ROW_FOCUS : COL_ROW);
            std::string label = "Device: (none found)";
            if (!drivers.empty()) {
                auto devs = devices_for(drivers[driver_idx], all_devices);
                if (!devs.empty()) {
                    label = "Device: " + devs[device_idx].name;
                }
            }
            draw_text(ui::font::fixed_5x8, kRowX + 4, kDeviceY + 4,
                      fit_text(ui::font::fixed_5x8, label, kRowW - 8), COL_TEXT,
                      sel ? COL_ROW_FOCUS : COL_ROW);
        }

        /* Controls & Display entry: opens the page with input mode, rotation,
         * wheel direction and the key-bindings editor. */
        {
            const bool sel = (focus == Focus::Controls);
            fill_rect(kRowX, kControlsY, kRowW, kControlsH, sel ? COL_ROW_FOCUS : COL_ROW);
            draw_text_center(ui::font::fixed_8x16, kRowX, kControlsY + 6, kRowW,
                             "Controls & Display", COL_TEXT, sel ? COL_ROW_FOCUS : COL_ROW);
        }

        /* Battery capacity: auto-detected, or a value typed by the user. */
        {
            const bool sel = (focus == Focus::Battery);
            fill_rect(kRowX, kBattY, kRowW, kBattH, sel ? COL_ROW_FOCUS : COL_ROW);
            const int mah = host_settings::battery_mah();
            const std::string label = (mah > 0)
                                          ? ("Battery: " + std::to_string(mah) + " mAh")
                                          : std::string("Battery: auto");
            draw_text_center(ui::font::fixed_8x16, kRowX, kBattY + 6, kRowW,
                             fit_text(ui::font::fixed_8x16, label, kRowW - 8),
                             COL_TEXT, sel ? COL_ROW_FOCUS : COL_ROW);
        }

        /* Bottom buttons. */
        {
            const bool sel = (focus == Focus::Start);
            fill_rect(kStartX, kBtnY, kStartW, kBtnH, COL_ACCENT);
            draw_text_center(ui::font::fixed_8x16, kStartX, kBtnY + 8, kStartW, "START",
                             rgb(10, 40, 15), COL_ACCENT);
            if (sel) {
                fill_rect(kStartX, kBtnY, kStartW, 2, COL_TEXT);
                fill_rect(kStartX, kBtnY + kBtnH - 2, kStartW, 2, COL_TEXT);
            }
        }
        {
            const bool sel = (focus == Focus::Quit);
            fill_rect(kQuitX, kBtnY, kQuitW, kBtnH, COL_QUIT);
            draw_text_center(ui::font::fixed_8x16, kQuitX, kBtnY + 8, kQuitW, "EXIT",
                             rgb(40, 10, 10), COL_QUIT);
            if (sel) {
                fill_rect(kQuitX, kBtnY, kQuitW, 2, COL_TEXT);
                fill_rect(kQuitX, kBtnY + kBtnH - 2, kQuitW, 2, COL_TEXT);
            }
        }

        lcd_sdl::render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    lcd_sdl::set_launcher_mode(false);

    /* Fill in the selected device. */
    if (!drivers.empty()) {
        result.driver = drivers[driver_idx];
        auto devs = devices_for(result.driver, all_devices);
        if (!devs.empty()) result.device_args = devs[device_idx].args;
    }
    result.touch_only = host_settings::touch_only();
    result.rotation = host_settings::rotation();

    return result;
}

int saved_rotation() { return host_settings::rotation(); }

}  // namespace host_launcher

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
/* Persisted host settings.                                                 */
/*                                                                        */
/* The only host-side setting saved between runs is the window rotation the  */
/* user picked in this window; it is kept in a tiny INI-ish file under the   */
/* XDG config dir so the next launch starts turned the same way.             */
/* ---------------------------------------------------------------------- */

fs::path config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME")) {
        base = fs::path(home) / ".config";
    } else {
        base = fs::path(".config");
    }
    return base / "mayhem-desktop" / "settings.conf";
}

int snap_rotation(int v) {
    v = ((v % 360) + 360) % 360;
    return (v / 90) * 90;
}

int load_saved_rotation() {
    std::ifstream in(config_path());
    if (!in) return 0;
    const std::string key = "rotation=";
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) == 0)
            return snap_rotation(std::atoi(line.c_str() + key.size()));
    }
    return 0;
}

void store_saved_rotation(int degrees) {
    std::error_code ec;
    const fs::path p = config_path();
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::trunc);
    if (out) out << "rotation=" << snap_rotation(degrees) << "\n";
}

/* ---------------------------------------------------------------------- */
/* Layout (panel pixels, 240x320 portrait).                                 */
/* ---------------------------------------------------------------------- */

constexpr int kRowX = 6;
constexpr int kRowW = host_lcd::WIDTH - 12;

constexpr int kBrowseY = 86;
constexpr int kBrowseH = 24;
constexpr int kDriverY = 126;
constexpr int kDriverH = 24;
constexpr int kDeviceY = 154;
constexpr int kDeviceH = 24;
constexpr int kModeLabelY = 186;
constexpr int kModeY = 194;
constexpr int kModeH = 30;
constexpr int kRotateLabelY = 230;
constexpr int kRotateY = 238;
constexpr int kRotateH = 28;
constexpr int kBtnY = 278;
constexpr int kBtnH = 34;
constexpr int kStartX = 6;
constexpr int kStartW = 112;
constexpr int kQuitX = 122;
constexpr int kQuitW = 112;

struct Focus {
    enum Value { Browse = 0, Driver, Device, Mode, Rotate, Start, Quit, Count };
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

}  // namespace

Result run(const std::string& default_sd, int scale) {
    Result result;
    result.start = false;
    result.sd_dir = default_sd;

    const std::vector<std::string> drivers = sdr::available_drivers();
    std::vector<sdr::DeviceInfo> all_devices = sdr::enumerate();

    int driver_idx = 0;
    int device_idx = 0;
    int focus = Focus::Browse;
    bool touch_only = false;  /* false = touch + on-screen buttons (default) */

    /* Window rotation, restored from the previous session's saved default and
     * applied immediately so the setup window is already turned. */
    int rotation_deg = load_saved_rotation();
    lcd_sdl::set_rotation(rotation_deg);
    auto cycle_rotation = [&](int dir) {
        int idx = (rotation_deg / 90 + dir + 4) % 4;
        rotation_deg = idx * 90;
        lcd_sdl::set_rotation(rotation_deg);
    };

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
                        } else if (focus == Focus::Mode) {
                            touch_only = !touch_only;
                        } else if (focus == Focus::Rotate) {
                            cycle_rotation(-1);
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
                        } else if (focus == Focus::Mode) {
                            touch_only = !touch_only;
                        } else if (focus == Focus::Rotate) {
                            cycle_rotation(+1);
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
                } else if (point_in(ev.x, ev.y, kRowX, kModeY, kRowW, kModeH)) {
                    focus = Focus::Mode;
                    touch_only = !touch_only;
                } else if (point_in(ev.x, ev.y, kRowX, kRotateY, kRowW, kRotateH)) {
                    focus = Focus::Rotate;
                    cycle_rotation(+1);
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
                    result.rotation = rotation_deg;
                    store_saved_rotation(rotation_deg);
                    done = true;
                    break;
                case Focus::Mode:
                    touch_only = !touch_only;
                    break;
                case Focus::Rotate:
                    cycle_rotation(+1);
                    break;
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

        /* Device section. */
        draw_text(ui::font::fixed_5x8, kRowX, 112, "SDR DEVICE", COL_SECTION, COL_BG);

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

        /* Input mode: the default keeps both the touchscreen and the on-screen
         * button deck; "touchscreen only" hides the deck so the window is just
         * the screen (top strip and handle stay). Toggled by tap or Left/Right. */
        draw_text(ui::font::fixed_5x8, kRowX, kModeLabelY, "INPUT MODE", COL_SECTION, COL_BG);
        {
            const bool sel = (focus == Focus::Mode);
            fill_rect(kRowX, kModeY, kRowW, kModeH, sel ? COL_ROW_FOCUS : COL_ROW);
            const std::string label = touch_only ? "Touchscreen only"
                                                 : "Touchscreen + buttons";
            draw_text(ui::font::fixed_5x8, kRowX + 4, kModeY + 4,
                      fit_text(ui::font::fixed_5x8, label, kRowW - 22), COL_TEXT,
                      sel ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + kRowW - 14, kModeY + 4,
                      "<>", COL_TEXT_DIM, sel ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + 4, kModeY + 15,
                      touch_only ? "screen only, no button deck" : "on-screen button deck shown",
                      COL_TEXT_DIM, sel ? COL_ROW_FOCUS : COL_ROW);
        }

        /* Window rotation, saved as the default for the next launch. */
        draw_text(ui::font::fixed_5x8, kRowX, kRotateLabelY, "WINDOW ROTATION", COL_SECTION, COL_BG);
        {
            const bool sel = (focus == Focus::Rotate);
            fill_rect(kRowX, kRotateY, kRowW, kRotateH, sel ? COL_ROW_FOCUS : COL_ROW);
            const char* names[4] = {"Portrait", "Landscape", "Portrait flipped", "Landscape flipped"};
            const std::string label = std::string("Rotation: ") + names[(rotation_deg / 90) % 4];
            draw_text(ui::font::fixed_5x8, kRowX + 4, kRotateY + 4,
                      fit_text(ui::font::fixed_5x8, label, kRowW - 22), COL_TEXT,
                      sel ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + kRowW - 14, kRotateY + 4,
                      "<>", COL_TEXT_DIM, sel ? COL_ROW_FOCUS : COL_ROW);
            draw_text(ui::font::fixed_5x8, kRowX + 4, kRotateY + 15,
                      "default, saved for next launch",
                      COL_TEXT_DIM, sel ? COL_ROW_FOCUS : COL_ROW);
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
    result.touch_only = touch_only;

    return result;
}

int saved_rotation() { return load_saved_rotation(); }

}  // namespace host_launcher

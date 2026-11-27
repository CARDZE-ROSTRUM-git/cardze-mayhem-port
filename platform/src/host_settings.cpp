/*
 * Host-side settings: see host_settings.hpp for the rationale.
 *
 * This translation unit needs SDL only for the default scancodes, and uses the
 * C library for the config file (no std::filesystem, so it is built normally).
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "host_settings.hpp"

/* hal.h (force-included) defines __I/__O/__IO as LPC43xx register qualifiers;
 * SDL's <immintrin.h> reuses those tokens. Drop them before SDL is seen. */
#undef __I
#undef __O
#undef __IO

#include <SDL.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace host_settings {

namespace {

constexpr int kActionCount = static_cast<int>(Action::Count);

struct Bindings {
    std::array<int32_t, kActionCount> keys;
    int rotation_deg{0};
    bool wheel_inv{false};
    bool touch_only{false};
    int battery_mah{0};
};

Bindings& store() {
    static Bindings b = [] {
        Bindings t;
        for (int i = 0; i < kActionCount; ++i) t.keys[i] = default_scancode(static_cast<Action>(i));
        return t;
    }();
    return b;
}

int snap_rotation(int v) {
    v = ((v % 360) + 360) % 360;
    return (v / 90) * 90;
}

/* Key names used in the settings file, one per action. */
const char* const kFileKeys[kActionCount] = {
    "key_nav_up",
    "key_nav_down",
    "key_nav_left",
    "key_nav_right",
    "key_select",
    "key_dfu",
    "key_encoder_cw",
    "key_encoder_ccw",
    "key_rotate",
    "key_fullscreen",
};

/* config_file() lives in the public header; the path is derived from XDG. */
}  // namespace

const char* action_label(Action a) {
    switch (a) {
        case Action::NavUp: return "Up";
        case Action::NavDown: return "Down";
        case Action::NavLeft: return "Left";
        case Action::NavRight: return "Right";
        case Action::Select: return "Select";
        case Action::Dfu: return "DFU";
        case Action::EncoderCW: return "Encoder CW";
        case Action::EncoderCCW: return "Encoder CCW";
        case Action::RotateWindow: return "Rotate window";
        case Action::ToggleFullscreen: return "Fullscreen";
        default: return "?";
    }
}

int32_t default_scancode(Action a) {
    switch (a) {
        case Action::NavUp: return SDL_SCANCODE_UP;
        case Action::NavDown: return SDL_SCANCODE_DOWN;
        case Action::NavLeft: return SDL_SCANCODE_LEFT;
        case Action::NavRight: return SDL_SCANCODE_RIGHT;
        case Action::Select: return SDL_SCANCODE_RETURN;
        case Action::Dfu: return SDL_SCANCODE_ESCAPE;
        case Action::EncoderCW: return SDL_SCANCODE_E;
        case Action::EncoderCCW: return SDL_SCANCODE_Q;
        case Action::RotateWindow: return SDL_SCANCODE_R;
        case Action::ToggleFullscreen: return SDL_SCANCODE_F;
        default: return 0;
    }
}

int32_t scancode(Action a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kActionCount) return 0;
    return store().keys[i];
}

void set_scancode(Action a, int32_t sdl_scancode) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kActionCount) return;
    store().keys[i] = sdl_scancode;
}

Action action_for_scancode(int32_t sdl_scancode) {
    if (sdl_scancode == 0) return Action::Count;
    for (int i = 0; i < kActionCount; ++i) {
        if (store().keys[i] == sdl_scancode) return static_cast<Action>(i);
    }
    return Action::Count;
}

bool scancode_is(Action a, int32_t sdl_scancode) {
    const int32_t bound = scancode(a);
    if (bound == sdl_scancode) return true;
    /* The two Enter keys are one physical intent; accept either for Select. */
    if (a == Action::Select) {
        const bool bound_enter = (bound == SDL_SCANCODE_RETURN || bound == SDL_SCANCODE_KP_ENTER);
        const bool got_enter = (sdl_scancode == SDL_SCANCODE_RETURN || sdl_scancode == SDL_SCANCODE_KP_ENTER);
        if (bound_enter && got_enter) return true;
    }
    return false;
}

void reset_key_defaults() {
    for (int i = 0; i < kActionCount; ++i)
        store().keys[i] = default_scancode(static_cast<Action>(i));
}

int rotation() { return store().rotation_deg; }
void set_rotation(int degrees) { store().rotation_deg = snap_rotation(degrees); }

bool touch_only() { return store().touch_only; }
void set_touch_only(bool on) { store().touch_only = on; }

bool wheel_inverted() { return store().wheel_inv; }
void set_wheel_inverted(bool inverted) { store().wheel_inv = inverted; }

int battery_mah() { return store().battery_mah; }
void set_battery_mah(int mah) {
    if (mah < 0) mah = 0;
    if (mah > 20000) mah = 20000; /* firmware's accepted upper bound */
    store().battery_mah = mah;
}

std::string config_file() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    } else {
        base = ".config";
    }
    return base + "/mayhem-desktop/settings.conf";
}

void load() {
    Bindings& b = store();
    std::ifstream in(config_file());
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "rotation") {
            b.rotation_deg = snap_rotation(std::atoi(val.c_str()));
        } else if (key == "wheel_inverted") {
            b.wheel_inv = (std::atoi(val.c_str()) != 0);
        } else if (key == "touch_only") {
            b.touch_only = (std::atoi(val.c_str()) != 0);
        } else if (key == "battery_mah") {
            int v = std::atoi(val.c_str());
            if (v < 0) v = 0;
            if (v > 20000) v = 20000;
            b.battery_mah = v;
        } else {
            for (int i = 0; i < kActionCount; ++i) {
                if (key == kFileKeys[i]) {
                    b.keys[i] = std::atoi(val.c_str());
                    break;
                }
            }
        }
    }
}

void save() {
    const std::string path = config_file();
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        /* Create the parent directory. std::filesystem would pull in the shim
         * question again; a plain mkdir through the C library is enough here and
         * keeps this file free of a second filesystem implementation. */
        const std::string dir = path.substr(0, slash);
        std::string acc;
        std::istringstream parts{dir};
        std::string part;
        while (std::getline(parts, part, '/')) {
            if (part.empty()) continue;
            acc += "/" + part;
            ::mkdir(acc.c_str(), 0755);
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    const Bindings& b = store();
    out << "rotation=" << b.rotation_deg << "\n";
    out << "wheel_inverted=" << (b.wheel_inv ? 1 : 0) << "\n";
    out << "touch_only=" << (b.touch_only ? 1 : 0) << "\n";
    out << "battery_mah=" << b.battery_mah << "\n";
    for (int i = 0; i < kActionCount; ++i)
        out << kFileKeys[i] << "=" << b.keys[i] << "\n";
}

}  // namespace host_settings

/*
 * Host replacement for firmware/common/battery.cpp.
 *
 * On the device the battery state comes from an I2C fuel-gauge chip (MAX17055)
 * or a voltage ADC (ADS1110) reached through I2CDevManager. The desktop has
 * neither, so isDetected() would always be false and the status bar would never
 * show a battery. This replacement reports the host's own battery instead, read
 * from the Linux power-supply class:
 *
 *   - /sys/class/power_supply/BAT.../   capacity, status, voltage_now
 *   - /sys/class/power_supply/AC.../    mains presence (type "Mains")
 *
 * So a laptop or a Cyberdeck shows its real charge, and a machine running on
 * mains (no battery, or a full/charging pack) reports 100 percent. A desktop
 * with no battery at all and no AC node still gets a plausible "on mains,
 * 100 percent" so the icon is present and consistent.
 *
 * The message is pushed to the UI on a 1 Hz cadence from host_battery::tick(),
 * called by the ~1 kHz host control tick; the firmware's own 1 Hz I2C thread
 * cannot do it here because no gauge chip is ever discovered.
 *
 * Note: this file is force-included with host_forced.hpp, which pulls in the
 * FatFs headers; those define a global "DIR" typedef, so <dirent.h> cannot be
 * used. The power-supply nodes are found with POSIX glob() instead, which does
 * not declare DIR.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "host_battery.hpp"

#include "battery.hpp"
#include "host_settings.hpp"

#include "event_m0.hpp"
#include "message.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glob.h>

namespace {

/* Reads a whole (small) sysfs attribute into `out`; returns false on failure.
 * Trims the trailing newline. */
bool read_sysfs(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return false;
    char buf[128];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    out.assign(buf);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return true;
}

/* Reads a decimal attribute as a signed integer; false if missing/empty. */
bool read_int_attr(const std::string& path, long& value) {
    std::string s;
    if (!read_sysfs(path, s) || s.empty()) return false;
    value = std::strtol(s.c_str(), nullptr, 10);
    return true;
}

/* Collects the power-supply directories matching a sysfs glob into `out`. */
void collect_dirs(const char* pattern, std::string& first_match) {
    glob_t g;
    if (glob(pattern, 0, nullptr, &g) != 0) {
        globfree(&g);
        return;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        if (first_match.empty()) {
            first_match = std::string(g.gl_pathv[i]) + "/";
        }
    }
    globfree(&g);
}

struct SupplyPaths {
    std::string battery; /* .../BATx/ directory, or empty */
    bool ac_online{false};
    bool ac_known{false};
};

/* Scans /sys/class/power_supply once and classifies the nodes. Cached for the
 * process because the set of supplies does not change at runtime. */
const SupplyPaths& supplies() {
    static SupplyPaths cached = [] {
        SupplyPaths p;

        /* Any BAT* node is a battery pack; the first one is used. */
        collect_dirs("/sys/class/power_supply/BAT*", p.battery);

        /* Any AC/ADP/USB/ACAD node whose type is "Mains" reports line power.
         * Different machines name these differently, so probe the common ones. */
        static const char* const kAcPatterns[] = {
            "/sys/class/power_supply/A*",
            "/sys/class/power_supply/U*",
        };
        for (const char* pattern : kAcPatterns) {
            glob_t g;
            if (glob(pattern, 0, nullptr, &g) != 0) {
                globfree(&g);
                continue;
            }
            for (size_t i = 0; i < g.gl_pathc; i++) {
                const std::string dir = std::string(g.gl_pathv[i]) + "/";

                /* Skip the battery node itself. */
                if (dir == p.battery) continue;

                std::string type;
                if (!read_sysfs(dir + "type", type)) continue;
                if (type != "Mains") continue;

                long online = 0;
                if (read_int_attr(dir + "online", online)) {
                    p.ac_known = true;
                    p.ac_online = p.ac_online || (online != 0);
                }
            }
            globfree(&g);
        }

        return p;
    }();
    return cached;
}

/* One sample of the host's power state, in the firmware's units. */
struct HostBattery {
    bool present{false};    /* a battery node exists */
    bool on_mains{false};   /* running from AC */
    uint8_t percent{100};
    uint16_t millivolts{0};
    uint16_t capacity_mah{0}; /* full-charge capacity reported by the pack, 0 if unknown */
    bool on_charger{false}; /* charging (or on mains with no battery) */
};

HostBattery sample() {
    HostBattery b;
    const SupplyPaths& s = supplies();

    b.on_mains = s.ac_known ? s.ac_online : false;

    if (!s.battery.empty()) {
        b.present = true;

        long capacity = 0;
        if (read_int_attr(s.battery + "capacity", capacity)) {
            if (capacity < 0) capacity = 0;
            if (capacity > 100) capacity = 100;
            b.percent = static_cast<uint8_t>(capacity);
        }

        long voltage_uv = 0;
        if (read_int_attr(s.battery + "voltage_now", voltage_uv) && voltage_uv > 0) {
            /* voltage_now is microvolts; the firmware wants millivolts. */
            b.millivolts = static_cast<uint16_t>(voltage_uv / 1000);
        }

        std::string status;
        if (read_sysfs(s.battery + "status", status)) {
            b.on_charger = (status == "Charging" || status == "Full");
        }

        /* Full-charge capacity straight from the pack, so the battery screen
         * shows the real mAh instead of the firmware's assumed default.
         * Prefer the pack's *design* (rated) capacity: the *_full value is the
         * worn full-charge capacity, which drops with age and looks like a
         * "remaining" figure. Only fall back to it if no design value exists.
         *   charge_* is in uAh -> /1000 = mAh
         *   energy_* is in uWh, so divide by the design voltage (uV) to get Ah
         *   and scale to mAh. */
        auto read_capacity = [&](const char* charge_attr, const char* energy_attr) -> long {
            long charge_uah = 0;
            if (read_int_attr(s.battery + charge_attr, charge_uah) && charge_uah > 0)
                return charge_uah / 1000;
            long energy_uwh = 0;
            if (read_int_attr(s.battery + energy_attr, energy_uwh) && energy_uwh > 0) {
                long v_uv = 0;
                if (!read_int_attr(s.battery + "voltage_min_design", v_uv) || v_uv <= 0)
                    read_int_attr(s.battery + "voltage_now", v_uv);
                if (v_uv > 0)
                    return static_cast<long>(static_cast<long long>(energy_uwh) * 1000 / v_uv);
            }
            return 0;
        };
        long cap_mah = read_capacity("charge_full_design", "energy_full_design");
        if (cap_mah <= 0) cap_mah = read_capacity("charge_full", "energy_full");
        if (cap_mah > 0) b.capacity_mah = static_cast<uint16_t>(cap_mah);
    }

    /* While the machine is on mains power, report a full pack: the operator asked
     * for "100 percent when plugged in". This also covers a desktop with no
     * battery. */
    if (b.on_mains || !b.present) {
        b.percent = 100;
        b.on_charger = true;
    }

    return b;
}

/* Last reported value, so the 1 Hz poll only pushes a message when something
 * changed (the upstream gauge did the same). */
uint8_t last_valid = 0xFF;
uint8_t last_percent = 0xFF;
bool last_charger = false;
bool first_send = true;

}  // namespace

namespace battery {

bool BatteryManagement::calcOverride = false;

void BatteryManagement::set_calc_override(bool override) {
    calcOverride = override;
}

bool BatteryManagement::isDetected() {
    /* The status bar only adds the battery icons when this is true. Report the
     * host's power supply as always present, so the icon shows even on a desktop
     * (mains -> 100 percent), matching what the user expects a "battery" to be. */
    return true;
}

uint8_t BatteryManagement::calc_percent_voltage(uint16_t voltage) {
    uint8_t batteryPercentage =
        (float)(voltage - BATTERY_MIN_VOLTAGE) / (float)(BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE) * 100.0;
    batteryPercentage = (batteryPercentage > 100) ? 100 : batteryPercentage;
    return batteryPercentage;
}

void BatteryManagement::getBatteryInfo(uint8_t& valid_mask, uint8_t& percent,
                                       uint16_t& voltage, int32_t& current,
                                       bool& battMayChanged) {
    const HostBattery b = sample();

    percent = b.percent;
    voltage = b.millivolts;
    valid_mask = BATT_VALID_PERCENT;
    if (b.millivolts > 0) valid_mask |= BATT_VALID_VOLTAGE;

    /* The firmware derives "on charger" from current >= 25 mA. Encode the host's
     * charging state the same way so on_battery_data() behaves identically. */
    current = b.on_charger ? 100 : -100;
    battMayChanged = false;
}

void BatteryManagement::reset_changed_flag() {}

uint16_t BatteryManagement::get_cycles() {
    return 0;
}

float BatteryManagement::get_tte() {
    return 0.0f;
}

float BatteryManagement::get_ttf() {
    return 0.0f;
}

uint16_t BatteryManagement::get_capacity_mah() {
    /* A value typed on the launcher's Battery line overrides everything. */
    const int override_mah = host_settings::battery_mah();
    if (override_mah > 0) return static_cast<uint16_t>(override_mah);
    /* Otherwise use the host's own pack full-charge capacity, so the battery
     * screen can show the real mAh instead of the firmware default. */
    return sample().capacity_mah;
}

}  // namespace battery

namespace host_battery {

/* Called from the ~1 kHz host control tick; pushes a BatteryStateMessage about
 * once a second, and immediately on the first call so the icon appears at boot. */
void tick() {
    static int counter = 0;
    /* host_controls_tick runs ~1 kHz; send on the first tick and then ~1 Hz. */
    if (!first_send && ++counter < 1000) return;
    counter = 0;

    uint8_t valid_mask = 0, percent = 0;
    uint16_t voltage = 0;
    int32_t current = 0;
    bool changed = false;
    battery::BatteryManagement::getBatteryInfo(valid_mask, percent, voltage, current, changed);

    const bool on_charger = current >= 25;
    if (!first_send && valid_mask == last_valid && percent == last_percent &&
        on_charger == last_charger) {
        return; /* nothing changed since the last push */
    }
    first_send = false;
    last_valid = valid_mask;
    last_percent = percent;
    last_charger = on_charger;

    BatteryStateMessage msg{valid_mask, percent, on_charger, voltage, changed};
    EventDispatcher::send_message(msg);
}

}  // namespace host_battery

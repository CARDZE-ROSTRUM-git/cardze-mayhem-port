/*
 * SDR device facade for the desktop port (see sdr_device.hpp).
 *
 * Keeps a table of the backends compiled into this build and dispatches
 * enumerate/open to them. Backends are referenced directly (no dynamic
 * registry), guarded by the feature macros the build sets, so an unused backend
 * pulls in no library.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "sdr_device.hpp"
#include "sdr_backends.hpp"

#include <string>
#include <vector>

namespace sdr {

namespace {

struct Backend {
    const char* driver;
    std::vector<DeviceInfo> (*enumerate)();
    std::unique_ptr<Device> (*open)(const std::string&);
};

/* Build-time table. Adding a backend means: implement its two hooks, add the
 * library to the link, and add a row here. */
const std::vector<Backend>& backends() {
    static const std::vector<Backend> table = {
#ifdef HOST_HAVE_RTLSDR
        {"rtlsdr", &rtlsdr_enumerate, &rtlsdr_open},
#endif
#ifdef HOST_HAVE_PLUTO
        {"pluto", &pluto_enumerate, &pluto_open},
#endif
    };
    return table;
}

}  // namespace

std::vector<std::string> available_drivers() {
    std::vector<std::string> out;
    for (const auto& b : backends()) out.emplace_back(b.driver);
    return out;
}

std::vector<DeviceInfo> enumerate() {
    std::vector<DeviceInfo> out;
    for (const auto& b : backends()) {
        auto devices = b.enumerate();
        for (auto& d : devices) out.push_back(std::move(d));
    }
    return out;
}

std::unique_ptr<Device> open(const std::string& driver, const std::string& args) {
    for (const auto& b : backends()) {
        if (driver == b.driver) return b.open(args);
    }
    return nullptr;
}

namespace {
std::string g_pref_driver;
std::string g_pref_args;
}  // namespace

void set_preferred_device(const std::string& driver, const std::string& args) {
    g_pref_driver = driver;
    g_pref_args = args;
}

void get_preferred_device(std::string& driver, std::string& args) {
    driver = g_pref_driver;
    args = g_pref_args;
}

std::unique_ptr<Device> open_preferred() {
    /* Explicit choice (launcher) wins. */
    if (!g_pref_driver.empty()) {
        if (auto device = open(g_pref_driver, g_pref_args)) return device;
    }

    /* No preference (e.g. launcher skipped): fall back to the first attached
     * device so the receive path still has a source. */
    for (const auto& info : enumerate()) {
        const std::string args = info.args.empty() ? std::string{} : info.args;
        if (auto device = open(info.driver, args)) return device;
    }
    return nullptr;
}

}  // namespace sdr

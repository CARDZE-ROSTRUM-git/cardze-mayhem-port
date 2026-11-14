/*
 * Host implementations for the firmware's hardware-facing entry points.
 *
 * This file is the "seam" the whole port is built around: the upstream UI,
 * apps and models keep calling radio::, ClockManager, audio::, the CPLD data
 * blobs and the core-control helpers exactly as before, but on the desktop
 * those symbols resolve here instead of to HackRF silicon drivers.
 *
 * Nothing in this file drives a HackRF. The radio entry points are deliberately
 * inert placeholders that record the requested configuration; stage 4 backs
 * them with SdrDevice (PlutoSDR via libiio / RTL-SDR via librtlsdr) so that the
 * very same calls tune a real radio instead of a fake one.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "hal.h"

#include "radio.hpp"
#include "clock_manager.hpp"
#include "si5351.hpp"
#include "rffc507x_spi.hpp"
#include "usb_serial.hpp"

#include "debug.hpp"
#include "core_control.hpp"

#include "rf_path.hpp"

#include "portapack_shared_memory.hpp"
#include "portapack_persistent_memory.hpp"
#include "spi_image.hpp"

#include "host_baseband.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

/* ------------------------------------------------------------------------- */
/* Linker-provided symbols the firmware reads directly (image layout, heap and */
/* the MPU stack guard). On the host they only need to exist with the right    */
/* addresses; the values are never touched by portable code at run time.       */
/*                                                                             */
/* These are declared by the upstream headers with C++ linkage (debug.hpp      */
/* declares __process_stack_*, spi_image.hpp declares _textend), so they must  */
/* be defined without an extern "C" wrapper to avoid a linkage clash.          */
/* ------------------------------------------------------------------------- */
uint32_t _textend = 0;
uint32_t __process_stack_base__ = 0;
uint32_t __process_stack_end__ = 0;

/* chibios_cpp.cpp measures the heap as __heap_end__ - __heap_base__ over two
 * byte arrays, so the symbols have to match that array declaration. The host
 * C++ runtime owns the real heap; these exist only so the arithmetic links. */
uint8_t __heap_base__[1] = {0};
uint8_t __heap_end__[1] = {0};

/* Set true for "r9" HackRF/PortaPack revisions (portapack.cpp reads it while
 * choosing PLL settings; on the host it only selects a constant branch). */
bool hackrf_r9 = false;

/* ------------------------------------------------------------------------- */
/* ChibiOS LPC43xx clock/reset helpers referenced by common/gpio.cpp.          */
/* The real implementation pokes CGU/CCU/RGU registers; here the register      */
/* writes land in plain host memory, so the helpers stay as documented above.  */
/* ------------------------------------------------------------------------- */
void peripheral_reset(const peripheral_reset_t* const reset) {
    (void)reset;
}

void base_clock_enable(const base_clock_regs_t* const base) {
    (void)base;
}

void base_clock_disable(const base_clock_regs_t* const base) {
    (void)base;
}

void branch_clock_enable(const branch_clock_regs_t* const branch) {
    (void)branch;
}

void branch_clock_disable(const branch_clock_regs_t* const branch) {
    (void)branch;
}

/* ------------------------------------------------------------------------- */
/* Radio seam.                                                                 */
/*                                                                             */
/* The upstream apps configure the radio through these calls. On the desktop   */
/* they cache the requested state and do nothing else: there is no HackRF      */
/* behind them. Stage 4 replaces the bodies with SdrDevice calls so that the   */
/* same tuning requests reach a PlutoSDR or an RTL-SDR for real.               */
/* ------------------------------------------------------------------------- */
namespace radio {

namespace {
/* Last requested configuration, kept for the future SdrDevice bridge and for
 * the debug screens that read it back. Plain fields: the calls arrive from the
 * single UI thread, and the value is inert until stage 4 wires up a radio. */
struct cached_state_t {
    rf::Frequency tuning_frequency{0};
    rf::Direction direction{rf::Direction::Receive};
    uint32_t baseband_rate{0};
    uint32_t baseband_filter_bandwidth{0};
    bool rf_amp{false};
    int_fast8_t lna_gain{0};
    int_fast8_t vga_gain{0};
    int_fast8_t tx_gain{0};
    bool antenna_bias{false};
} cached_state;
}  // namespace

/* Read-only accessors for the host baseband core (see host_radio namespace
 * below). The cached state lives in an anonymous namespace so portable code
 * cannot name it; these give the host-only glue a way to read it. */
rf::Frequency cached_tuning_frequency_for_host() {
    return cached_state.tuning_frequency;
}

uint32_t cached_baseband_rate_for_host() {
    return cached_state.baseband_rate;
}

int cached_lna_gain_for_host() {
    return cached_state.lna_gain;
}

int cached_vga_gain_for_host() {
    return cached_state.vga_gain;
}

bool cached_rf_amp_for_host() {
    return cached_state.rf_amp;
}

uint32_t cached_baseband_filter_bandwidth_for_host() {
    return cached_state.baseband_filter_bandwidth;
}

bool cached_antenna_bias_for_host() {
    return cached_state.antenna_bias;
}

rf::Direction cached_direction_for_host() {
    return cached_state.direction;
}

int cached_tx_gain_for_host() {
    return cached_state.tx_gain;
}

void init() {
    cached_state = cached_state_t{};
}

void set_direction(const rf::Direction new_direction) {
    cached_state.direction = new_direction;
}

bool set_tuning_frequency(const rf::Frequency frequency) {
    /* Mirror upstream radio::set_tuning_frequency()'s pre-processing, because
     * firmware/application/radio.cpp is not built on the host (it drives the
     * HackRF first-IF/second-IF chips). The up/down converter and the RX/TX
     * frequency corrections are pure persistent-memory arithmetic, so applying
     * them here keeps the converter toggle and the Settings corrections working
     * against the real SDR backend. */
    rf::Frequency final_frequency = frequency;

    if (portapack::persistent_memory::config_converter()) {
        if (portapack::persistent_memory::config_updown_converter()) {
            final_frequency = frequency - portapack::persistent_memory::config_converter_freq();
        } else {
            final_frequency = frequency + portapack::persistent_memory::config_converter_freq();
        }
    }

    if (cached_state.direction == rf::Direction::Transmit) {
        if (portapack::persistent_memory::config_freq_tx_correction_updown())
            final_frequency = final_frequency - portapack::persistent_memory::config_freq_tx_correction();
        else
            final_frequency = final_frequency + portapack::persistent_memory::config_freq_tx_correction();
    } else {
        if (portapack::persistent_memory::config_freq_rx_correction_updown())
            final_frequency = final_frequency - portapack::persistent_memory::config_freq_rx_correction();
        else
            final_frequency = final_frequency + portapack::persistent_memory::config_freq_rx_correction();
    }

    cached_state.tuning_frequency = final_frequency;
    return true;
}
void set_fast_retune(const bool enabled) {
    (void)enabled;
}

void set_rf_amp(const bool rf_amp) {
    cached_state.rf_amp = rf_amp;
}

void set_lna_gain(const int_fast8_t db) {
    cached_state.lna_gain = db;
}

void set_vga_gain(const int_fast8_t db) {
    cached_state.vga_gain = db;
}

void set_tx_gain(const int_fast8_t db) {
    cached_state.tx_gain = db;
}

void set_baseband_filter_bandwidth_rx(const uint32_t bandwidth_minimum) {
    cached_state.baseband_filter_bandwidth = bandwidth_minimum;
}

void set_baseband_filter_bandwidth_tx(const uint32_t bandwidth_minimum) {
    cached_state.baseband_filter_bandwidth = bandwidth_minimum;
}

void set_baseband_rate(const uint32_t rate) {
    cached_state.baseband_rate = rate;
}

void set_antenna_bias(const bool on) {
    cached_state.antenna_bias = on;
}

bool get_mixer_invert() {
    return false;
}

bool get_baseband_invert() {
    return false;
}

void set_tx_max283x_iq_phase_calibration(const size_t v) {
    (void)v;
}

void set_rx_max283x_iq_phase_calibration(const size_t v) {
    (void)v;
}

void disable() {
}

namespace debug {

namespace first_if {

uint32_t register_read(const size_t register_number) {
    (void)register_number;
    return 0;
}

void register_write(const size_t register_number, uint32_t value) {
    (void)register_number;
    (void)value;
}

}  // namespace first_if

namespace second_if {

uint32_t register_read(const size_t register_number) {
    (void)register_number;
    return 0;
}

void register_write(const size_t register_number, uint32_t value) {
    (void)register_number;
    (void)value;
}

int8_t temp_sense() {
    return 0;
}

}  // namespace second_if

namespace rf_path_info {

rf::path::Band get_current_band() {
    return rf::path::Band::Low;
}

}  // namespace rf_path_info

namespace sgpio {

uint32_t register_read(const size_t register_number) {
    (void)register_number;
    return 0;
}

}  // namespace sgpio

}  // namespace debug

}  // namespace radio

/* ------------------------------------------------------------------------- */
/* Read-only view of the radio seam's cached state for the host baseband core  */
/* (platform/src/host_baseband.cpp). Kept here, next to cached_state.          */
/* ------------------------------------------------------------------------- */
namespace host_radio {

uint64_t tuning_frequency() {
    return (uint64_t)radio::cached_tuning_frequency_for_host();
}

uint32_t baseband_rate() {
    return radio::cached_baseband_rate_for_host();
}

int lna_gain_db() {
    return radio::cached_lna_gain_for_host();
}

int vga_gain_db() {
    return radio::cached_vga_gain_for_host();
}

bool rf_amp() {
    return radio::cached_rf_amp_for_host();
}

uint32_t baseband_filter_bandwidth() {
    return radio::cached_baseband_filter_bandwidth_for_host();
}

bool antenna_bias() {
    return radio::cached_antenna_bias_for_host();
}

int tx_gain_db() {
    return radio::cached_tx_gain_for_host();
}

bool transmitting() {
    return radio::cached_direction_for_host() == rf::Direction::Transmit;
}

}  // namespace host_radio

/* ------------------------------------------------------------------------- */
/* ClockManager.                                                               */
/*                                                                             */
/* The upstream implementation programs the Si5351 and the LPC43xx CGU. The    */
/* host build has neither, so the methods report a fixed, plausible reference  */
/* (the on-board 10 MHz Xtal) and otherwise do nothing. The reference source is */
/* read by the navigation bar, the settings screen and the receiver UI, so it   */
/* must return a real value.                                                   */
/* ------------------------------------------------------------------------- */
void ClockManager::init_clock_generator() {}

void ClockManager::shutdown() {}

void ClockManager::start_audio_pll() {}

void ClockManager::stop_audio_pll() {}

void ClockManager::set_base_audio_clock_divider(const size_t divisor) {
    (void)divisor;
}

void ClockManager::enable_codec_clocks() {}

void ClockManager::disable_codec_clocks() {}

void ClockManager::enable_if_clocks() {}

void ClockManager::disable_if_clocks() {}

void ClockManager::set_sampling_frequency(const uint32_t frequency) {
    _base_band_frequency = frequency;
}

void ClockManager::set_reference_ppb(const int32_t ppb) {
    (void)ppb;
}

uint32_t ClockManager::get_frequency_monitor_measurement_in_hertz() {
    return reference.frequency;
}

ClockManager::Reference ClockManager::get_reference() const {
    return reference;
}

std::string ClockManager::get_source() {
    return "Xtal";
}

std::string ClockManager::get_freq() {
    return "10.0";
}

void ClockManager::enable_clock_output(bool enable) {
    (void)enable;
}

void ClockManager::portapack_tcxo_enable() {}

void ClockManager::portapack_tcxo_disable() {}

/* ------------------------------------------------------------------------- */
/* CPLD configuration blobs.                                                   */
/*                                                                             */
/* Upstream these arrays are generated from the CPLD bitstream by               */
/* tools/extract_cpld_data.py and programmed over JTAG. The desktop never has   */
/* a CPLD, but ui_settings.cpp takes their address, so the symbols must exist.  */
/* ------------------------------------------------------------------------- */
namespace portapack {
namespace cpld {
namespace rev_20150901 {
/* Must carry `extern`: the upstream headers declare these as extern const, and
 * a bare namespace-scope const would get internal linkage. */
extern const std::array<uint16_t, 3328> block_0;
extern const std::array<uint16_t, 512> block_1;
const std::array<uint16_t, 3328> block_0{};
const std::array<uint16_t, 512> block_1{};
}  // namespace rev_20150901
namespace rev_20170522 {
extern const std::array<uint16_t, 3328> block_0;
extern const std::array<uint16_t, 512> block_1;
const std::array<uint16_t, 3328> block_0{};
const std::array<uint16_t, 512> block_1{};
}  // namespace rev_20170522
}  // namespace cpld
}  // namespace portapack

/* ------------------------------------------------------------------------- */
/* Baseband core control (M0/M4 image loading and reloading).                  */
/*                                                                             */
/* On the device this copies baseband images into the M4 code region and        */
/* restarts the cores. The host has neither core, so these are inert: they      */
/* report success and mark the (emulated) baseband as ready, which is what the  */
/* real M4 would do once its image is running. baseband_api::run_image() blocks */
/* up to 3 s waiting for shared_memory.baseband_ready and then panics with      */
/* "Baseband Sync Fail"; setting it here keeps every app that starts a baseband */
/* image (which is most of them) running instead of aborting.                   */
/* ------------------------------------------------------------------------- */
bool m4_init(const portapack::spi_flash::image_tag_t image_tag, const portapack::memory::region_t to, const bool full_reset) {
    (void)to;
    (void)full_reset;
    /* Tell the host data plane which image is now running; it decides where a
     * CaptureConfig is routed (capture IQ ring vs. audio output ring). */
    using portapack::spi_flash::image_tag_am_audio;
    using portapack::spi_flash::image_tag_capture;
    using portapack::spi_flash::image_tag_nfm_audio;
    using portapack::spi_flash::image_tag_wfm_audio;
    using portapack::spi_flash::image_tag_replay;
    using portapack::spi_flash::image_tag_audio_tx;
    host_baseband::Image image = host_baseband::Image::Other;
    if (image_tag == image_tag_capture) {
        image = host_baseband::Image::Capture;
    } else if (image_tag == image_tag_am_audio ||
               image_tag == image_tag_nfm_audio ||
               image_tag == image_tag_wfm_audio) {
        image = host_baseband::Image::Audio;
    } else if (image_tag == image_tag_replay) {
        image = host_baseband::Image::Replay;
    } else if (image_tag == image_tag_audio_tx) {
        image = host_baseband::Image::AudioTX;
    }
    host_baseband::set_image(image);

    shared_memory.set_baseband_ready();
    return true;
}

void m4_init_prepared(const uint32_t m4_code, const bool full_reset) {
    (void)m4_code;
    (void)full_reset;
    shared_memory.set_baseband_ready();
}

void m4_set_bundled_reloader(bool (*reload)()) {
    (void)reload;
}

void m4_restore_bundled() {}

void m0_halt() {}

/* ------------------------------------------------------------------------- */
/* RFFC507x register access used by the debug screens.                        */
/* The upstream driver talks to the real chip over bit-banged SPI; here the    */
/* chip is absent, so power-up/down are inert. (Si5351::read_register is        */
/* provided by the upstream si5351.cpp, which is built as-is.)                  */
/* ------------------------------------------------------------------------- */
void rffc507x::spi::SPI::power_down() {}

void rffc507x::spi::SPI::init() {}

/* ------------------------------------------------------------------------- */
/* USB serial shell hooks.                                                     */
/*                                                                             */
/* create_shell_i2c() installs the I2C-bridge shell used to talk to a host over */
/* the device's USB serial port, and complete_i2chost_to_device_transfer()      */
/* finishes one such transfer. There is no device USB on the desktop, so the    */
/* shell is never created and the completion hook is a no-op. Both are declared */
/* with C linkage upstream, so the definitions must match.                      */
/* ------------------------------------------------------------------------- */
class EventDispatcher;

extern "C" void create_shell_i2c(EventDispatcher* evtd) {
    (void)evtd;
}

extern "C" void complete_i2chost_to_device_transfer(uint8_t* data, size_t length) {
    (void)data;
    (void)length;
}

/* ------------------------------------------------------------------------- */
/* portapack::USBSerial methods.                                               */
/* The upstream implementation drives the LPC43xx USB controller. The desktop  */
/* build keeps the class interface (event_m0.cpp dispatches transfers) but does */
/* not enumerate a USB device yet; all entry points are inert.                 */
/* ------------------------------------------------------------------------- */
namespace portapack {

void USBSerial::initialize() {}

void USBSerial::dispatch() {}

void USBSerial::dispatch_transfer() {}

void USBSerial::on_channel_opened() {}

void USBSerial::on_channel_closed() {}

}  // namespace portapack

/* ------------------------------------------------------------------------- */
/* Crash/debug helpers.                                                        */
/*                                                                             */
/* draw_guru_meditation() paints the fatal-error screen when a core faults;    */
/* stack_dump()/memory_dump() write diagnostic files from the debug app. None  */
/* of them can do anything useful (or harmful) on the host.                    */
/* ------------------------------------------------------------------------- */
struct extctx;

void draw_guru_meditation(uint8_t core_id, const char* message) {
    std::fprintf(stderr, "mayhem-desktop: core %u fault: %s\n",
                 static_cast<unsigned>(core_id), message ? message : "(null)");
}

void draw_guru_meditation(uint8_t core_id, const char* message, struct extctx* ctx, uint32_t extra) {
    (void)ctx;
    (void)extra;
    draw_guru_meditation(core_id, message);
}

bool stack_dump() {
    return false;
}

bool memory_dump(uint32_t* addr_start, uint32_t num_words, bool stack_flag) {
    (void)addr_start;
    (void)num_words;
    (void)stack_flag;
    return false;
}

/* ------------------------------------------------------------------------- */
/* RTC driver.                                                                 */
/*                                                                             */
/* The firmware keeps the wall clock in the LPC43xx RTC and reads it through    */
/* rtc_time::now(). Rather than a dead stub, back RTCD1 with the host's own     */
/* clock so timestamps, the clock app and the on-screen time are real.          */
/* ------------------------------------------------------------------------- */
RTCDriver RTCD1;

namespace {

/* Packs a broken-down wall-clock time into the ChibiOS RTCTime layout used by
 * the firmware: date = (year << 16) | (month << 8) | day, time = (hour << 16) |
 * (minute << 8) | second. */
RTCTime rtctime_from_tm(const struct tm& t) {
    RTCTime out;
    out.tv_date = (static_cast<uint32_t>(t.tm_year + 1900) << 16) |
                  (static_cast<uint32_t>(t.tm_mon + 1) << 8) |
                  static_cast<uint32_t>(t.tm_mday);
    out.tv_time = (static_cast<uint32_t>(t.tm_hour) << 16) |
                  (static_cast<uint32_t>(t.tm_min) << 8) |
                  static_cast<uint32_t>(t.tm_sec);
    return out;
}

}  // namespace

void rtcGetTime(RTCDriver* rtc, RTCTime* timespec) {
    (void)rtc;
    if (!timespec) return;
    /* Upstream stores the wall clock (local time) in the RTC: rtc_time.cpp notes
     * that DST is only subtracted because the stored value is non-DST local
     * time, i.e. "If RTC is desired to represent UTC, then DST should obviously
     * be disabled". So present the host's *local* time, which is what the
     * on-screen clock and the desktop panel both show. */
    const std::time_t now = std::time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    *timespec = rtctime_from_tm(t);
}

void rtcSetTime(RTCDriver* rtc, const RTCTime* timespec) {
    (void)rtc;
    (void)timespec;
    /* The host clock is the single source of truth; writes are ignored. */
}

/* rtc_wakeup() puts the device to sleep for N seconds and wakes on the RTC
 * alarm. On the host the UI thread simply sleeps when asked. */
void rtc_wakeup_init() {}

void rtc_wakeup(uint32_t sleep_seconds) {
    (void)sleep_seconds;
}

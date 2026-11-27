/*
 * Host-side SDR backend abstraction for the desktop port.
 *
 * The firmware talks to a MAX2837 (HackRF) through SPI and receives its base
 * band samples from the SGPIO/GPDMA pair: a steady stream of complex8_t
 * (interleaved signed-8-bit I and Q), 8192 samples per buffer. Everything above
 * that point - DSP, spectrum, demodulators, the whole application layer - is
 * portable C++ that only ever sees that int8 IQ stream.
 *
 * On the desktop we keep that contract exactly and swap the hardware under it:
 * a small device interface feeds the same interleaved int8 IQ into the baseband
 * DMA shim, so every bit of the firmware's receive path runs unmodified.
 *
 * Two drivers are provided:
 *   - "rtlsdr": RTL2832U dongles via librtlsdr. Already 8-bit IQ, this is the
 *     bring-up target that can be exercised against real hardware.
 *   - "pluto":  ADALM-Pluto / Zynq7010 via libiio. The primary target for this
 *     port; 16-bit IQ is reduced to 8-bit on the way in.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __HOST_SDR_DEVICE_HPP__
#define __HOST_SDR_DEVICE_HPP__

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace sdr {

/* One enumerated device, as shown in the launcher's device list. */
struct DeviceInfo {
    std::string driver;  /* "rtlsdr" or "pluto" */
    std::string args;    /* driver-specific hint: USB index for rtlsdr, URI for pluto */
    std::string name;    /* human readable, e.g. "Generic RTL2832U" */
    std::string serial;
};

/* Probe every compiled-in backend and return the devices it can see. */
std::vector<DeviceInfo> enumerate();

/* Identifiers of the backends this build has compiled in ("rtlsdr", "pluto"). */
std::vector<std::string> available_drivers();

/*
 * A live radio. Sample I/O always returns interleaved signed 8-bit IQ
 * (I, Q, I, Q, ...), matching baseband::sample_t, so callers can hand the raw
 * bytes straight to the firmware DSP.
 */
class Device {
   public:
    virtual ~Device() = default;

    /* Bring the device up with the given args (driver-specific). */
    virtual bool open(const std::string& args) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual bool set_sample_rate(double hz) = 0;
    virtual double sample_rate() const = 0;

    virtual bool set_center_freq(double hz) = 0;
    virtual double center_freq() const = 0;

    /* Manual gain, in dB. Returns false if the driver cannot honour it. */
    virtual bool set_gain(double db) = 0;
    virtual bool set_gain_mode(bool manual) = 0;
    /* Front-end bandwidth in Hz (0 = let the driver choose). */
    virtual bool set_bandwidth(double hz) = 0;

    /*
     * The tuner's manual-gain range in dB. The host baseband maps the
     * firmware's HackRF LNA+VGA setting onto this span. A driver that cannot
     * report a range leaves both at 0, which the caller treats as "no mapping,
     * use the value as-is".
     */
    virtual void gain_range(double& min_db, double& max_db) {
        (void)min_db;
        (void)max_db;
        min_db = 0.0;
        max_db = 0.0;
    }

    /*
     * True when this radio has no analogue equivalent of the HackRF's LNA/VGA
     * stages and should run its own automatic gain control instead. A cheap
     * DVB-T tuner (RTL) spans only a few dB, so mapping the firmware's 0..116 dB
     * LNA+VGA span onto it leaves the default setting near the noise floor and
     * decoders never see a signal. The host baseband then enables AGC once and
     * ignores the per-app LNA/VGA values. Devices with a real gain span (Pluto)
     * keep the proportional mapping and return false here.
     */
    virtual bool prefers_auto_gain() const { return false; }

    /* Bias tee / antenna power, where supported. */
    virtual bool set_bias_tee(bool on) = 0;

    /*
     * Read up to max_samples complex samples (2 * max_samples bytes) into dst.
     * Blocks until data is available or the device is stopped. Returns the
     * number of complex samples actually read, or 0 on stop/error.
     */
    virtual size_t read(int8_t* dst, size_t max_samples) = 0;

    /* Ask a blocking read() to return. Safe to call from another thread. */
    virtual void stop() = 0;

    /*
     * Transmit support. A driver that cannot transmit leaves tx_supported()
     * false; the host baseband then drains the application's TX ring without
     * pretending the samples left the antenna. Everything else in this block
     * returns false/0 in that case, so callers need only test tx_supported().
     */
    virtual bool tx_supported() const { return false; }

    /*
     * TX local oscillator. On an FDD transceiver (AD936x) this is independent of
     * the RX one, so a full-duplex app can hold both at once.
     */
    virtual bool set_tx_frequency(double hz) {
        (void)hz;
        return false;
    }

    /* TX gain in dB (driver-specific span; the caller clamps its own range). */
    virtual bool set_tx_gain(double db) {
        (void)db;
        return false;
    }

    /*
     * Write up to count complex samples (2 * count bytes) from src: interleaved
     * signed 8-bit IQ, the same baseband::sample_t the receive path uses. Blocks
     * until the samples are queued. Returns the number of complex samples
     * accepted, or 0 on error/unsupported. One producer thread at a time.
     */
    virtual size_t write(const int8_t* src, size_t count) {
        (void)src;
        (void)count;
        return 0;
    }

    /* Ask a blocking write() to return. Safe to call from another thread. */
    virtual void stop_tx() {}
};

/* Open a device by driver name and args. Returns nullptr on failure. */
std::unique_ptr<Device> open(const std::string& driver, const std::string& args);

/* Records the driver/args chosen in the setup window, so the baseband bring-up
 * (which has no arguments of its own) can open the right device later. */
void set_preferred_device(const std::string& driver, const std::string& args);

/* Returns the recorded choice (empty strings if none was set). */
void get_preferred_device(std::string& driver, std::string& args);

/* Opens the recorded device. Returns nullptr if none was set or it fails. */
std::unique_ptr<Device> open_preferred();

}  // namespace sdr

#endif /*__HOST_SDR_DEVICE_HPP__*/

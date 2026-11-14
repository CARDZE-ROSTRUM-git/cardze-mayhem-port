/*
 * ADALM-Pluto / Zynq7010 backend for the desktop PortaPack SDR layer.
 *
 * This is the primary target of the port. The Pluto runs Linux on its Zynq7010
 * and exposes its AD936x transceiver through libiio: a "cf-ad9361-lpc" RX
 * device streams complex samples, while the "ad9361-phy" device holds the LO
 * frequency, sample rate, RF bandwidth and gain controls. libiio talks to the
 * hardware either locally (when this program runs on the Pluto itself) or over
 * the network/USB via a URI, which is how a desktop reaches it.
 *
 * Receive and transmit are separate IIO devices: "cf-ad9361-dds-core-lpc" is the
 * TX DMA/streaming device, with its own LO (altvoltage1) so RX and TX can be
 * tuned independently (FDD). TX takes the same interleaved signed 8-bit IQ the
 * firmware produces and widens it to the 16-bit little-endian words the TX DMA
 * expects.
 *
 * The Pluto delivers 12-bit samples packed into 16-bit little-endian words; the
 * firmware baseband expects interleaved signed 8-bit IQ, so each sample is
 * shifted down by 8 bits (a cheap, exact divide-by-256 that preserves the top
 * bits and full scale). Everything above this file sees the same byte stream
 * the HackRF SGPIO used to provide.
 *
 * NOTE: there is no Pluto attached to the current development machine, so this
 * backend has not been exercised against hardware. It is written strictly to
 * the documented libiio / Pluto attribute set so it can be validated as soon as
 * a unit is available.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "sdr_backends.hpp"

#include <iio.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sdr {
namespace {

/* The Pluto RX streaming device and its physical control device. */
constexpr const char* kRxDevice = "cf-ad9361-lpc";
constexpr const char* kPhyDevice = "ad9361-phy";

/* RX I/Q physical channel names and the RX local oscillator (altvoltage0). */
constexpr const char* kRxI = "voltage0";
constexpr const char* kRxQ = "voltage1";
constexpr const char* kRxLO = "altvoltage0";

/* TX streaming device. On the Pluto the transmit path is a separate IIO device
 * ("cf-ad9361-dds-core-lpc") with its own output channels and a second LO
 * (altvoltage1 on ad9361-phy), so RX and TX can be tuned independently (FDD). */
constexpr const char* kTxDevice = "cf-ad9361-dds-core-lpc";
constexpr const char* kTxI = "voltage0";
constexpr const char* kTxQ = "voltage1";
constexpr const char* kTxLO = "altvoltage1";

class PlutoDevice : public Device {
   public:
    ~PlutoDevice() override { close(); }

    bool open(const std::string& args) override {
        close();

        /* Prefer the URI the caller gave (ip:..., usb:..., or empty for a local
         * context). Each URI form has an alias in the scan list too. */
        if (!args.empty()) {
            ctx_ = iio_create_context_from_uri(args.c_str());
        } else {
            ctx_ = iio_create_default_context();
            if (ctx_ == nullptr) ctx_ = iio_create_local_context();
        }
        if (ctx_ == nullptr) return false;

        iio_context_set_timeout(ctx_, 2000);

        rx_dev_ = iio_context_find_device(ctx_, kRxDevice);
        phy_dev_ = iio_context_find_device(ctx_, kPhyDevice);
        if (rx_dev_ == nullptr || phy_dev_ == nullptr) {
            close();
            return false;
        }

        i_chn_ = iio_device_find_channel(rx_dev_, kRxI, false);
        q_chn_ = iio_device_find_channel(rx_dev_, kRxQ, false);
        lo_chn_ = iio_device_find_channel(phy_dev_, kRxLO, true); /* TX channel of the RX LO */
        phy_chn_ = iio_device_find_channel(phy_dev_, kRxI, false); /* PHY RF voltage0 for gain */

        if (i_chn_ == nullptr || q_chn_ == nullptr || lo_chn_ == nullptr) {
            close();
            return false;
        }

        iio_channel_enable(i_chn_);
        iio_channel_enable(q_chn_);

        /* TX path is optional: an RX-only context (or a driver that hides the
         * TX device) still yields a usable receive-only radio. */
        tx_dev_ = iio_context_find_device(ctx_, kTxDevice);
        if (tx_dev_ != nullptr) {
            tx_i_chn_ = iio_device_find_channel(tx_dev_, kTxI, true);
            tx_q_chn_ = iio_device_find_channel(tx_dev_, kTxQ, true);
            tx_lo_chn_ = iio_device_find_channel(phy_dev_, kTxLO, true);
            tx_gain_chn_ = iio_device_find_channel(phy_dev_, kTxI, true);
            if (tx_i_chn_ == nullptr || tx_q_chn_ == nullptr) {
                tx_dev_ = nullptr;
                tx_i_chn_ = tx_q_chn_ = tx_lo_chn_ = tx_gain_chn_ = nullptr;
            } else {
                iio_channel_enable(tx_i_chn_);
                iio_channel_enable(tx_q_chn_);
            }
        }

        open_ = true;
        return true;
    }

    void close() override {
        stop();
        stop_tx();
        if (buf_ != nullptr) {
            iio_buffer_destroy(buf_);
            buf_ = nullptr;
        }
        if (ctx_ != nullptr) {
            iio_context_destroy(ctx_);
            ctx_ = nullptr;
        }
        rx_dev_ = phy_dev_ = tx_dev_ = nullptr;
        i_chn_ = q_chn_ = lo_chn_ = phy_chn_ = nullptr;
        tx_i_chn_ = tx_q_chn_ = tx_lo_chn_ = tx_gain_chn_ = nullptr;
        open_ = false;
    }

    bool is_open() const override { return open_; }

    bool set_sample_rate(double hz) override {
        if (phy_dev_ == nullptr) return false;
        return iio_device_attr_write_longlong(phy_dev_, "sampling_frequency",
                                              static_cast<long long>(hz)) >= 0;
    }

    double sample_rate() const override {
        long long v = 0;
        if (phy_dev_ != nullptr &&
            iio_device_attr_read_longlong(phy_dev_, "sampling_frequency", &v) >= 0) {
            return static_cast<double>(v);
        }
        return 0.0;
    }

    bool set_center_freq(double hz) override {
        if (lo_chn_ == nullptr) return false;
        return iio_channel_attr_write_longlong(lo_chn_, "frequency",
                                               static_cast<long long>(hz)) >= 0;
    }

    double center_freq() const override {
        long long v = 0;
        if (lo_chn_ != nullptr &&
            iio_channel_attr_read_longlong(lo_chn_, "frequency", &v) >= 0) {
            return static_cast<double>(v);
        }
        return 0.0;
    }

    bool set_gain(double db) override {
        if (phy_chn_ == nullptr) return false;
        /* Pluto wants manual gain mode before a hardwaregain can be set. */
        iio_channel_attr_write(phy_chn_, "gain_control_mode", "manual");
        return iio_channel_attr_write_longlong(phy_chn_, "hardwaregain",
                                               static_cast<long long>(db)) >= 0;
    }

    bool set_gain_mode(bool manual) override {
        if (phy_chn_ == nullptr) return false;
        return iio_channel_attr_write(phy_chn_, "gain_control_mode",
                                      manual ? "manual" : "slow_attack") >= 0;
    }

    void gain_range(double& min_db, double& max_db) override {
        /* The AD936x manual-gain range is 0..73 dB in 1 dB steps. libiio does
         * not expose the min/max through a plain attribute read, so report the
         * datasheet span and let set_gain() clamp into it. */
        min_db = 0.0;
        max_db = 73.0;
    }

    bool set_bandwidth(double hz) override {
        if (phy_dev_ == nullptr) return false;
        return iio_device_attr_write_longlong(phy_dev_, "rf_bandwidth",
                                              static_cast<long long>(hz)) >= 0;
    }

    bool set_bias_tee(bool /*on*/) override {
        /* The Pluto has no standard bias-tee control through libiio. */
        return false;
    }

    size_t read(int8_t* dst, size_t max_samples) override {
        if (!open_) return 0;

        if (buf_ == nullptr) {
            buf_ = iio_device_create_buffer(rx_dev_, kBufferSamples, false);
            if (buf_ == nullptr) return 0;
        }

        ssize_t nbytes = iio_buffer_refill(buf_);
        if (nbytes < 0) return 0;

        /* Each complex sample is two little-endian int16 (I then Q). */
        const uint8_t* p = static_cast<const uint8_t*>(iio_buffer_start(buf_));
        const size_t raw_samples = static_cast<size_t>(nbytes) / 4;
        const size_t count = raw_samples < max_samples ? raw_samples : max_samples;

        for (size_t i = 0; i < count; ++i) {
            const int16_t vi = static_cast<int16_t>(p[i * 4 + 0] | (p[i * 4 + 1] << 8));
            const int16_t vq = static_cast<int16_t>(p[i * 4 + 2] | (p[i * 4 + 3] << 8));
            dst[i * 2 + 0] = static_cast<int8_t>(vi >> 8);
            dst[i * 2 + 1] = static_cast<int8_t>(vq >> 8);
        }
        return count;
    }

    void stop() override {
        if (buf_ != nullptr) {
            iio_buffer_destroy(buf_);
            buf_ = nullptr;
        }
    }

    /* ---- Transmit (AD936x TX / cf-ad9361-dds-core-lpc) ---- */

    bool tx_supported() const override { return tx_dev_ != nullptr; }

    bool set_tx_frequency(double hz) override {
        if (tx_lo_chn_ == nullptr) return false;
        return iio_channel_attr_write_longlong(tx_lo_chn_, "frequency",
                                               static_cast<long long>(hz)) >= 0;
    }

    bool set_tx_gain(double db) override {
        if (tx_gain_chn_ == nullptr) return false;
        /* The TX channel only accepts a hardwaregain once it is in manual mode. */
        iio_channel_attr_write(tx_gain_chn_, "gain_control_mode", "manual");
        return iio_channel_attr_write_longlong(tx_gain_chn_, "hardwaregain",
                                               static_cast<long long>(db)) >= 0;
    }

    size_t write(const int8_t* src, size_t count) override {
        if (!open_ || tx_dev_ == nullptr || count == 0) return 0;

        if (tx_buf_ == nullptr) {
            tx_buf_ = iio_device_create_buffer(tx_dev_, kBufferSamples, false);
            if (tx_buf_ == nullptr) return 0;
        }

        uint8_t* p = static_cast<uint8_t*>(iio_buffer_start(tx_buf_));
        /* The TX DMA buffer holds 16-bit little-endian IQ. The firmware hands us
         * signed 8-bit IQ; scale it up by 256 so full scale maps to full scale
         * (the AD936x ignores the low 4 bits of each 12-bit sample anyway). */
        const size_t n = count < kBufferSamples ? count : kBufferSamples;
        for (size_t i = 0; i < n; ++i) {
            const int16_t vi = static_cast<int16_t>(src[i * 2 + 0]) << 8;
            const int16_t vq = static_cast<int16_t>(src[i * 2 + 1]) << 8;
            p[i * 4 + 0] = static_cast<uint8_t>(vi & 0xFF);
            p[i * 4 + 1] = static_cast<uint8_t>((vi >> 8) & 0xFF);
            p[i * 4 + 2] = static_cast<uint8_t>(vq & 0xFF);
            p[i * 4 + 3] = static_cast<uint8_t>((vq >> 8) & 0xFF);
        }

        const ssize_t pushed = iio_buffer_push(tx_buf_);
        if (pushed < 0) return 0;
        return n;
    }

    void stop_tx() override {
        if (tx_buf_ != nullptr) {
            iio_buffer_destroy(tx_buf_);
            tx_buf_ = nullptr;
        }
    }

   private:
    /* One Pluto RX/TX buffer: 2^18 samples ~= 43 ms at 6.144 MS/s. */
    static constexpr unsigned int kBufferSamples = 1u << 18;

    iio_context* ctx_{nullptr};
    iio_device* rx_dev_{nullptr};
    iio_device* phy_dev_{nullptr};
    iio_device* tx_dev_{nullptr};
    iio_channel* i_chn_{nullptr};
    iio_channel* q_chn_{nullptr};
    iio_channel* lo_chn_{nullptr};
    iio_channel* phy_chn_{nullptr};
    iio_channel* tx_i_chn_{nullptr};
    iio_channel* tx_q_chn_{nullptr};
    iio_channel* tx_lo_chn_{nullptr};
    iio_channel* tx_gain_chn_{nullptr};
    iio_buffer* buf_{nullptr};
    iio_buffer* tx_buf_{nullptr};
    bool open_{false};
};

}  // namespace

std::vector<DeviceInfo> pluto_enumerate() {
    std::vector<DeviceInfo> out;

    /* USB ADALM-Pluto uses the default libiio context / standard USB URI; a
     * networked Pluto is reached with "ip:<host>". Report the USB context and,
     * if the environment names a remote host, that one too. */
    auto scan = iio_create_scan_context("usb", 0);
    if (scan != nullptr) {
        iio_context_info** info = nullptr;
        const ssize_t n = iio_scan_context_get_info_list(scan, &info);
        for (ssize_t i = 0; i < n; ++i) {
            DeviceInfo dev;
            dev.driver = "pluto";
            dev.args = ""; /* default context picks the USB device */
            const char* desc = iio_context_info_get_description(info[i]);
            dev.name = desc ? desc : "PlutoSDR";
            out.push_back(std::move(dev));
        }
        if (n > 0) iio_context_info_list_free(info);
        iio_scan_context_destroy(scan);
    }

    /* Also try a plain default context (covers a local/on-Pluto build). */
    if (out.empty()) {
        iio_context* ctx = iio_create_default_context();
        if (ctx == nullptr) ctx = iio_create_local_context();
        if (ctx != nullptr) {
            if (iio_context_find_device(ctx, kRxDevice) != nullptr) {
                DeviceInfo dev;
                dev.driver = "pluto";
                dev.args = "";
                dev.name = "PlutoSDR (local)";
                out.push_back(std::move(dev));
            }
            iio_context_destroy(ctx);
        }
    }

    return out;
}

std::unique_ptr<Device> pluto_open(const std::string& args) {
    auto dev = std::make_unique<PlutoDevice>();
    if (!dev->open(args)) return nullptr;
    return dev;
}

}  // namespace sdr

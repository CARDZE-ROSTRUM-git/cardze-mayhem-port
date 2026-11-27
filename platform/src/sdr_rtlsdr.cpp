/*
 * RTL2832U backend for the desktop PortaPack SDR layer (see sdr_device.hpp).
 *
 * RTL dongles already deliver interleaved unsigned 8-bit IQ; the firmware's
 * baseband pipeline wants interleaved *signed* 8-bit IQ (complex8_t), so every
 * sample is biased from [0,255] to [-128,127] on the way in. Everything above
 * this file sees exactly the byte stream the HackRF SGPIO used to provide.
 *
 * Samples are pulled with rtlsdr_read_async() into a ring buffer that a
 * consumer thread drains through read(); this hides USB's bursty delivery from
 * the baseband loop, which expects a steady buffer every 8192 samples.
 *
 * This is the bring-up target: it runs against real hardware today.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "sdr_backends.hpp"

#include <rtl-sdr.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sdr {
namespace {

/* Ring buffer sized for a comfortable few tens of milliseconds of headroom at
 * 3.2 MS/s: 1 << 20 complex samples = 2 MiB of int8 IQ. */
constexpr size_t kRingSamples = 1u << 20;
constexpr size_t kRingMask = kRingSamples - 1;

class RtlDevice : public Device {
   public:
    ~RtlDevice() override { close(); }

    bool open(const std::string& args) override {
        close();

        uint32_t index = 0;
        if (!args.empty()) index = static_cast<uint32_t>(std::stoul(args));

        if (rtlsdr_open(&dev_, index) != 0 || dev_ == nullptr) {
            dev_ = nullptr;
            return false;
        }

        /* DC/1/f are handled downstream by the firmware's DC blocking, so plain
         * zero-IF is fine; disable the RTL's own offset tuning. Both AGCs are
         * left OFF: the RTL2832U *digital* AGC pins the 8-bit stream at +-127,
         * and the tuner's own auto gain (R820T) slams to maximum on weak or
         * empty input, saturating the waterfall (solid red/yellow) and flooding
         * every decoder with noise. Gain is driven manually from the apps'
         * LNA/VGA sliders through set_gain()/prefers_auto_gain(). */
        rtlsdr_set_offset_tuning(dev_, 0);
        rtlsdr_set_agc_mode(dev_, 0);

        ring_.assign(kRingSamples * 2, 0); /* bytes: complex samples x 2 */
        open_ = true;
        return true;
    }

    void close() override {
        if (!open_) {
            dev_ = nullptr;
            return;
        }
        stop();
        if (dev_ != nullptr) {
            rtlsdr_close(dev_);
            dev_ = nullptr;
        }
        open_ = false;
    }

    bool is_open() const override { return open_; }

    bool set_sample_rate(double hz) override {
        if (!open_) return false;
        uint32_t want = static_cast<uint32_t>(hz);

        /* The RTL2832U tuner IF has a "dead band" between roughly 300 kHz and
         * 900 kHz that the PLL cannot lock: asking for a rate there makes
         * librtlsdr print "Invalid sample rate" and fail. The firmware's
         * oversample rates can land in it (e.g. capture at 200 kHz x4 = 800 kHz),
         * so snap the request up to a known-good rate instead of failing - the
         * caller reads the applied rate back through sample_rate(). */
        if (want > 300000u && want < 1000000u) want = 1024000u;
        if (want < 250000u) want = 250000u;

        if (rtlsdr_set_sample_rate(dev_, want) != 0) {
            /* Unknown tuner with different limits: try the common good rates. */
            for (const uint32_t candidate : {1024000u, 1536000u, 2048000u}) {
                if (rtlsdr_set_sample_rate(dev_, candidate) == 0) break;
            }
        }
        rate_ = rtlsdr_get_sample_rate(dev_);
        return true;
    }

    double sample_rate() const override { return rate_; }

    bool set_center_freq(double hz) override {
        if (!open_) return false;
        if (rtlsdr_set_center_freq(dev_, static_cast<uint32_t>(hz)) != 0) return false;
        center_ = rtlsdr_get_center_freq(dev_);
        return true;
    }

    double center_freq() const override { return center_; }

    bool set_gain(double db) override {
        if (!open_) return false;
        rtlsdr_set_tuner_gain_mode(dev_, 1);
        /* librtlsdr wants tenths of a dB. */
        return rtlsdr_set_tuner_gain(dev_, static_cast<int>(db * 10.0)) == 0;
    }

    bool set_gain_mode(bool manual) override {
        if (!open_) return false;
        /* manual == false selects the tuner's own AGC, which is what
         * prefers_auto_gain() asks the host baseband to request. */
        const int rc = rtlsdr_set_tuner_gain_mode(dev_, manual ? 1 : 0);
        return rc == 0;
    }

    /* Drive the tuner with a manual gain mapped from the apps' LNA/VGA sliders.
     * The RTL is a cheap DVB-T tuner whose span is smaller than the HackRF's
     * 116 dB chain, but running its own auto gain saturates the front end (the
     * waterfall goes solid red/yellow), so a mapped manual gain is used. */
    bool prefers_auto_gain() const override { return false; }

    void gain_range(double& min_db, double& max_db) override {
        min_db = 0.0;
        max_db = 0.0;
        if (!open_) return;
        /* Ask the tuner for its discrete gain table; librtlsdr returns the
         * count. The first and last entries (tenths of a dB) bound the range. */
        int count = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (count <= 0) return;
        std::vector<int> gains(static_cast<size_t>(count));
        if (rtlsdr_get_tuner_gains(dev_, gains.data()) <= 0) return;
        min_db = gains.front() / 10.0;
        max_db = gains.back() / 10.0;
    }

    bool set_bandwidth(double hz) override {
        if (!open_) return false;
        return rtlsdr_set_tuner_bandwidth(dev_, static_cast<uint32_t>(hz)) == 0;
    }

    bool set_bias_tee(bool on) override {
        if (!open_) return false;
        return rtlsdr_set_bias_tee(dev_, on ? 1 : 0) == 0;
    }

    size_t read(int8_t* dst, size_t max_samples) override {
        if (!open_) return 0;

        ensure_streaming();

        std::unique_lock<std::mutex> lock(mutex_);
        data_cv_.wait(lock, [&] {
            return stopped_.load(std::memory_order_relaxed) ||
                   stream_ended_.load(std::memory_order_relaxed) ||
                   (write_pos_ - read_pos_) > 0;
        });

        if (stopped_.load(std::memory_order_relaxed)) return 0;
        /* The USB stream thread finished (dongle unplugged / driver error): no
         * more samples will ever arrive, so report the failure instead of
         * blocking forever, which lets the caller drop and reopen the device.
         * Do NOT clear streaming_ here: the caller's close() -> stop() still has
         * to join the finished stream thread. */
        if (stream_ended_.load(std::memory_order_relaxed) &&
            (write_pos_ - read_pos_) == 0) {
            return 0;
        }

        const size_t available = write_pos_ - read_pos_;
        const size_t take = available < max_samples ? available : max_samples;

        const size_t ri = read_pos_ & kRingMask;
        const size_t first = (kRingSamples - ri) < take ? (kRingSamples - ri) : take;
        std::memcpy(dst, ring_.data() + ri * 2, first * 2);
        if (first < take) {
            std::memcpy(reinterpret_cast<uint8_t*>(dst) + first * 2, ring_.data(),
                        (take - first) * 2);
        }
        read_pos_ += take;
        lock.unlock();

        return take;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!streaming_) return;
            stopped_.store(true, std::memory_order_relaxed);
        }
        if (dev_ != nullptr) rtlsdr_cancel_async(dev_);
        data_cv_.notify_all();

        if (stream_thread_.joinable()) stream_thread_.join();
        streaming_ = false;
        stream_ended_.store(false, std::memory_order_relaxed);
    }

   private:
    static void async_cb(unsigned char* buf, uint32_t len, void* ctx) {
        static_cast<RtlDevice*>(ctx)->on_samples(buf, len);
    }

    /* Called on librtlsdr's USB thread, from inside libusb's event loop. It must
     * NEVER block: while it runs the USB thread holds libusb's internal event
     * lock, so waiting here deadlocks any concurrent libusb transfer. The
     * producer changes center frequency / sample rate from its own thread via
     * libusb_control_transfer, which needs that same lock - if the ring were
     * allowed to block the callback, set_center_freq() would hang forever while
     * the callback waited for the producer to drain. Instead the ring never
     * blocks: when it is full the oldest samples are dropped, so the consumer
     * always sees the freshest data. */
    void on_samples(const unsigned char* buf, uint32_t len) {
        if (stopped_.load(std::memory_order_relaxed)) return;
        size_t samples = len / 2;
        if (samples == 0) return;
        if (samples > kRingSamples) {
            /* A single block larger than the ring: keep only its newest part. */
            buf += (samples - kRingSamples) * 2;
            samples = kRingSamples;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        /* Drop the oldest samples when this block would overflow the ring. */
        if ((write_pos_ - read_pos_) + samples > kRingSamples) {
            read_pos_ = write_pos_ + samples - kRingSamples;
        }

        const size_t wi = write_pos_ & kRingMask;
        const size_t first = (kRingSamples - wi) < samples ? (kRingSamples - wi) : samples;
        bias_into(ring_.data() + wi * 2, buf, first * 2);
        if (first < samples) {
            bias_into(ring_.data(), buf + first * 2, (samples - first) * 2);
        }
        write_pos_ += samples;
        data_cv_.notify_all();
    }

    /* unsigned [0,255] -> signed [-128,127] */
    static void bias_into(uint8_t* dst, const unsigned char* src, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            dst[i] = static_cast<uint8_t>(static_cast<int>(src[i]) - 128);
        }
    }

    void ensure_streaming() {
        if (streaming_) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (streaming_) return;
            write_pos_ = read_pos_ = 0;
            stopped_.store(false, std::memory_order_relaxed);
            stream_ended_.store(false, std::memory_order_relaxed);
            streaming_ = true;
        }

        rtlsdr_reset_buffer(dev_);
        stream_thread_ = std::thread([this] {
            rtlsdr_read_async(dev_, &RtlDevice::async_cb, this, 0, 0);
            /* rtlsdr_read_async() returns when the stream is cancelled (stop())
             * or when the USB transfer fails (dongle removed). In the latter
             * case nobody would otherwise wake the consumer, so raise the flag
             * and signal it; read() then reports the failure. */
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_ended_.store(true, std::memory_order_relaxed);
            }
            data_cv_.notify_all();
        });
    }

    rtlsdr_dev_t* dev_{nullptr};
    bool open_{false};
    std::atomic<bool> streaming_{false};
    double rate_{0.0};
    double center_{0.0};

    std::thread stream_thread_;
    std::vector<uint8_t> ring_;
    size_t write_pos_{0};
    size_t read_pos_{0};
    std::mutex mutex_;
    std::condition_variable data_cv_;  /* consumer waits for data */
    std::atomic<bool> stopped_{false};
    std::atomic<bool> stream_ended_{false}; /* USB thread returned unexpectedly */
};

}  // namespace

std::vector<DeviceInfo> rtlsdr_enumerate() {
    std::vector<DeviceInfo> out;
    const uint32_t n = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < n; ++i) {
        DeviceInfo info;
        info.driver = "rtlsdr";
        info.args = std::to_string(i);
        const char* name = rtlsdr_get_device_name(i);
        info.name = name ? name : "RTL-SDR";
        char manufact[256] = {0}, product[256] = {0}, serial[256] = {0};
        if (rtlsdr_get_device_usb_strings(i, manufact, product, serial) == 0) {
            info.serial = serial;
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::unique_ptr<Device> rtlsdr_open(const std::string& args) {
    auto dev = std::make_unique<RtlDevice>();
    if (!dev->open(args)) return nullptr;
    return dev;
}

}  // namespace sdr

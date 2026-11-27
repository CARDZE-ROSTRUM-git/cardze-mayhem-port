/*
 * Host emulation of the M4 baseband core's receive path.
 *
 * On the device the M4 runs a per-image processor. For capture/spectrum that is
 * CaptureProcessor (firmware/baseband/proc_capture.cpp): it pulls buffers of
 * complex8_t IQ from the SGPIO/GPDMA pair, runs the firmware's decimation chain
 * over them, feeds a SpectrumCollector for the waterfall and, when recording,
 * writes the decimated complex16_t samples into a StreamInput ring that the
 * application-side CaptureThread drains.
 *
 * The desktop has no M4 and no SGPIO, so this module plays its part, using the
 * firmware's *own* building blocks - the same decimators (dsp::decimate::...)
 * and the same FFT (common/dsp_fft.hpp) - so the arithmetic is the device's,
 * not a reimplementation:
 *
 *   - It feeds them from the host SDR backend (sdr::open_preferred()), pulling
 *     interleaved signed 8-bit IQ: exactly the byte stream the SGPIO provided.
 *   - On a CaptureConfig it creates the StreamInput the application expects, so
 *     RecordView's CaptureThread sees real FIFOs instead of null pointers (the
 *     crash the missing M4 caused), and it writes the decimated samples there.
 *   - On SpectrumStreamingConfig it hands the UI its spectrum FIFO and computes
 *     a waterfall frame from each decimated block.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "host_baseband.hpp"
#include "host_radio.hpp"
#include "sdr_device.hpp"
#include "host_m4.hpp"

#include "message.hpp"
#include "portapack_shared_memory.hpp"
#include "buffer_exchange.hpp"
#include "event_m0.hpp"

/* Must precede dsp_decimate.hpp: provides the ARM intrinsics and pre-defines
 * the firmware/common/simd.hpp guard so the DSP sources see this shim. */
#include "host_simd.hpp"

#include "host_audio.hpp"

#include "spec_an_shared.hpp"
#include "spec_an_dsp.hpp"

#include "dsp_types.hpp"
#include "dsp_decimate.hpp"
#include "dsp_demodulate.hpp"
#include "dsp_frequency_xlator.hpp"
#include "dsp_fir_taps.hpp"
#include "dsp_fft.hpp"
#include "dsp_iir.hpp"
#include "dsp_squelch.hpp"
#include "audio_compressor.hpp"
#include "block_decimator.hpp"
#include "utility.hpp"
#include "stream_input.hpp"
#include "stream_output.hpp"
#include "replay_schedule.hpp"
#include "dsp_replay_interpolate.hpp"
#include "tone_gen.hpp"
#include "sine_table_int8.hpp"

#include "ch.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace host_baseband {

namespace {

/* One DMA transfer as the firmware's baseband_thread sees it. baseband_dma.cpp
 * splits its 8192-sample buffer into transfers_per_buffer (4) LLI transfers, so
 * execute() receives 8192 / 4 = 2048 complex8_t. The decimators' 512-sample dst
 * is sized for exactly this (Decim4 of 2048 -> 512). Feeding a larger block
 * overruns dst and corrupts adjacent state. */
constexpr size_t kBufferSamples = 2048;

/* Local no-op decimator (firmware keeps this in proc_capture.hpp, whose other
 * members pull in the M4 thread machinery the host does not need). */
class NoopDecim {
   public:
    static constexpr int decimation_factor = 1;

    template <typename Buffer>
    Buffer execute(const Buffer& src, const Buffer&) {
        return {src.p, src.count, src.sampling_rate};
    }
};

std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running{false};
bool g_started = false;

uint32_t g_sample_rate = 0; /* output (decimated) rate */
uint32_t g_oversample = 1;
uint32_t g_baseband_fs = 0; /* rate the SDR delivers */

std::array<complex16_t, 512> g_dst{};
const buffer_c16_t g_dst_buffer{g_dst.data(), g_dst.size()};

/* Two decimation stages, mirroring CaptureProcessor. Only the configured stage
 * is active; the unused member simply accumulates no output. */
int g_stage = 0; /* 0: decim4; 1: decim8 */
dsp::decimate::FIRC8xR16x24FS4Decim4 g_d0_dec4{};
dsp::decimate::FIRC8xR16x24FS4Decim8 g_d0_dec8{};
int g_stage1 = 0; /* 0: noop; 1: decim2; 2: decim8 */
dsp::decimate::FIRC16xR16x16Decim2 g_d1_dec2{};
dsp::decimate::FIRC16xR16x32Decim8 g_d1_dec8{};
NoopDecim g_d1_noop{};

int32_t g_filter_low_f = 0;
int32_t g_filter_high_f = 0;
int32_t g_filter_transition = 0;

std::unique_ptr<StreamInput> g_stream{};

/* Spectrum path (waterfall). */
std::atomic<bool> g_streaming{false};
ChannelSpectrum g_spectrum_fifo_data[1 << ChannelSpectrumConfigMessage::fifo_k]{};
ChannelSpectrumFIFO g_spectrum_fifo{g_spectrum_fifo_data, ChannelSpectrumConfigMessage::fifo_k};
std::array<complex16_t, 256> g_fft_raw{};
std::array<std::complex<float>, 256> g_fft_input{};
size_t g_fft_fill = 0;
uint32_t g_fft_sampling_rate = 0;

/* Spectrum frame pacing, copied verbatim from CaptureProcessor. The M4 does NOT
 * emit a waterfall row every 256 decimated samples; it paces frames at a fixed
 * spectrum_rate_hz with a credit scheme, so the four-deep ChannelSpectrumFIFO
 * is drained by the ~60 Hz UI frame clock instead of overflowing and dropping
 * rows (which makes the waterfall jump instead of scroll smoothly). */
constexpr float kSpectrumRateHz = 50.0f;
size_t g_spectrum_interval_samples = 0;
size_t g_spectrum_samples = 0;
uint16_t g_spectrum_capture_remaining = 0;
uint16_t g_spectrum_capture_credit = 0;

/* Three-term Hamming window, as in the firmware's SpectrumCollector. */
inline std::complex<float> window_hamming_3(const std::array<std::complex<float>, 256>& s, const size_t i) {
    constexpr size_t N = 256;
    constexpr size_t mask = N - 1;
    return s[i] * 0.54f + (s[(i - 1) & mask] + s[(i + 1) & mask]) * -0.23f;
}

void feed_spectrum() {
    /* Bit-reverse into the FFT scratch, then run the firmware FFT - exactly the
     * fft_swap + fft_c_preswapped pair SpectrumCollector::update() uses. */
    fft_swap(g_fft_raw, g_fft_input);
    fft_c_preswapped(g_fft_input, 0, 8);

    ChannelSpectrum spectrum;
    spectrum.sampling_rate = g_fft_sampling_rate;
    spectrum.channel_filter_low_frequency = g_filter_low_f;
    spectrum.channel_filter_high_frequency = g_filter_high_f;
    spectrum.channel_filter_transition = g_filter_transition;

    for (size_t i = 0; i < spectrum.db.size(); i++) {
        const auto corrected = window_hamming_3(g_fft_input, i);
        const float mag2 = std::norm(corrected * (1.0f / 32768.0f));
        const float db = mag2_to_dbv_norm(mag2);
        const float v = (db * 5.0f) + 255.0f;
        const int vi = (int)v;
        spectrum.db[i] = (uint8_t)std::max(0, std::min(255, vi));
    }

    g_spectrum_fifo.in(spectrum);

    /* Wake the UI so it drains the FIFO on its next frame. */
    EventDispatcher::check_fifo_isr();
}

/* Fallback waterfall feed for images that never run the receive chain. While
 * the radio transmits (the Replay/Playlist/Remote apps, which all load the
 * replay image) or idles on a backend without a TX path, process()/emit() are
 * never reached, so the app's WaterfallView would sit on its cleared black
 * rectangle - the dead black band seen when opening Remote. Feed a dim,
 * slightly noisy floor at the same ~50 Hz frame rate so the band scrolls and
 * the app looks alive. Harmless for RX: it only runs when streaming is on and
 * no real spectrum is being produced. */
void feed_floor_spectrum() {
    ChannelSpectrum spectrum;
    spectrum.sampling_rate = 0;
    spectrum.channel_filter_low_frequency = 0;
    spectrum.channel_filter_high_frequency = 0;
    spectrum.channel_filter_transition = 0;
    static uint32_t s = 0x1234567u;
    for (size_t i = 0; i < spectrum.db.size(); i++) {
        s = s * 1664525u + 1013904223u;
        /* A narrow band around a low level (about -43 dBv), so the gradient
         * paints a dim but clearly non-black floor. */
        spectrum.db[i] = static_cast<uint8_t>(44 + ((s >> 25) & 0x07));
    }
    g_spectrum_fifo.in(spectrum);
    EventDispatcher::check_fifo_isr();
}

/* Rate-limits feed_floor_spectrum() to the waterfall's ~50 Hz frame clock when
 * called from the tight transmit/idle loops of the producer thread. */
void feed_floor_if_due() {
    if (!g_streaming) return;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(20)) return;
    last = now;
    feed_floor_spectrum();
}

/* Mirror of CaptureProcessor::feed_spectrum: copy at most the samples left for
 * the pending frame into the 256-point FFT accumulator, completing exactly one
 * frame. Returns true when a frame completed (so the caller clears the grant),
 * false while still accumulating. */
bool feed_channel_spectrum(const buffer_c16_t& buffer) {
    if (!g_spectrum_capture_remaining || !buffer.count) return false;

    const size_t take = std::min<size_t>(buffer.count, g_spectrum_capture_remaining);
    bool completed = false;

    for (size_t i = 0; i < take; i++) {
        g_fft_raw[g_fft_fill++] = buffer.p[i];
        if (g_fft_fill == g_fft_raw.size()) {
            g_fft_fill = 0;
            g_fft_sampling_rate = buffer.sampling_rate;
            feed_spectrum();
            completed = true;
        }
    }

    if (completed) {
        g_spectrum_capture_remaining = 0;
    } else {
        g_spectrum_capture_remaining -= take;
    }
    return completed;
}

void spectrum_reset() {
    g_fft_raw.fill({});
    g_fft_input.fill({0.0f, 0.0f});
    g_fft_fill = 0;

    /* CaptureProcessor::reset_spectrum_capture(), minus the lock: the host's
     * producer already holds g_mutex across configure/process. */
    g_spectrum_samples = 0;
    g_spectrum_capture_remaining = 0;
    g_spectrum_capture_credit = 0;
}

/* ------------------------------------------------------------------------- */
/* Swept spectrum engine (SpecAn).                                             */
/*                                                                             */
/* Host port of firmware/baseband/spec_an_engine.cpp. The M0 side of the       */
/* protocol (spec_an::Sweep, already compiled into the app) retunes the tuner  */
/* per slice and bumps Shared::req_seq; this engine captures the requested     */
/* DMA buffers, runs the firmware's own FFT/window/detector, and posts         */
/* SpecAnCaptured/SpecAnSlice back to the application queue. The upstream      */
/* engine splits capture (baseband thread) and FFT (event thread); the host    */
/* runs both on the producer thread, right after the samples arrive.           */
/* ------------------------------------------------------------------------- */

class HostSpecAnEngine {
   public:
    void configure(const SpecAnConfigMessage& m) {
        configured_ = false;
        shared_ = m.shared;
        gen_ = m.gen;
        log2n_ = std::clamp<size_t>(m.fft_log2n, spec_an::kMinFftLog2, spec_an::kMaxFftLog2);
        n_ = size_t{1} << log2n_;
        blocks_per_buffer_ = spec_an::kDmaBufferSamples >> log2n_;
        avg_blocks_ = std::max<size_t>(m.avg_blocks, 1);
        bin_first_ = m.bin_first;
        bins_per_slice_ = std::clamp<size_t>(m.bins_per_slice, 1,
                                             std::min(n_ - 16, spec_an::kMaxSliceBins));
        slices_ = std::max<size_t>(m.slices, 1);
        window_ = m.window;

        const float px_per_bin =
            static_cast<float>(m.px_per_bin_num) / static_cast<float>(m.px_per_bin_den);
        detector_.configure(px_per_bin, m.detector,
                            spec_an::dsp::norm_db(n_, window_, avg_blocks_), slices_);

        for (auto& slot : slots_) slot.full = false;
        slot_wr_ = 0;
        slot_rd_ = 0;
        capturing_ = false;
        captured_pending_ = false;
        armed_seq_ = shared_->req_seq;
        acc_valid_ = false;

        shared_->waterfall = waterfall_;
        configured_ = true;
        std::fprintf(stderr,
                     "host_spec_an: configured gen=%u rate=%u log2n=%u k=%u slices=%u avg=%u\n",
                     (unsigned)gen_, (unsigned)m.sampling_rate, (unsigned)log2n_,
                     (unsigned)bins_per_slice_, (unsigned)slices_, (unsigned)avg_blocks_);
    }

    void stop() { configured_ = false; }
    bool active() const { return configured_; }

    void execute(const buffer_c8_t& buffer) {
        if (!configured_) return;

        if (captured_pending_) post_captured();

        const uint32_t seq = shared_->req_seq;
        if (seq != armed_seq_) {
            armed_seq_ = seq;
            cap_slice_ = shared_->req_slice;
            cap_sweep_ = shared_->req_sweep;
            settle_left_ = shared_->req_settle;
            blocks_left_ = avg_blocks_;
            capturing_ = true;
            captured_pending_ = false; /* superseded */
        }

        if (!capturing_) return;
        if (settle_left_) {
            settle_left_--;
            return;
        }
        /* The engine expects whole DMA buffers (2048 complex8_t). */
        if (buffer.count < spec_an::kDmaBufferSamples) return;

        Slot& slot = slots_[slot_wr_];
        if (slot.full) return; /* consumer is behind, take the next buffer */

        const size_t blocks = std::min(blocks_left_, blocks_per_buffer_);
        std::memcpy(slot_samples_[slot_wr_], buffer.p,
                    (blocks << log2n_) * sizeof(complex8_t));
        slot.seq = seq;
        slot.slice = cap_slice_;
        slot.sweep = cap_sweep_;
        slot.blocks = static_cast<uint16_t>(blocks);
        slot.last = (blocks == blocks_left_);
        slot.full = true;
        slot_wr_ ^= 1;

        blocks_left_ -= blocks;
        if (blocks_left_ == 0) {
            capturing_ = false;
            captured_seq_ = seq;
            captured_pending_ = true;
            post_captured();
        }
    }

    void process_pending() {
        if (!configured_) return;

        for (size_t iter = 0; iter < slots_.size(); iter++) {
            Slot& slot = slots_[slot_rd_];
            if (!slot.full) break;

            const uint32_t seq = slot.seq;
            const uint16_t slice = slot.slice;
            const uint8_t sweep = slot.sweep;
            const size_t blocks = slot.blocks;
            const bool last = slot.last;
            const int8_t* const samples =
                reinterpret_cast<const int8_t*>(slot_samples_[slot_rd_]);

            if (!acc_valid_ || seq != acc_seq_) {
                std::fill_n(pow_acc_, bins_per_slice_, 0.0f);
                acc_seq_ = seq;
                acc_valid_ = true;
            }

            for (size_t b = 0; b < blocks; b++) {
                spec_an::dsp::load_block(fft_mem_, samples + 2 * (b << log2n_), log2n_);
                if (b + 1 == blocks) {
                    slot.full = false;
                    slot_rd_ ^= 1;
                }
                spec_an::dsp::fft(fft_mem_, log2n_, fft_sin_q_);
                spec_an::dsp::window_power(fft_mem_, n_, bin_first_, bins_per_slice_,
                                           window_, pow_acc_);
            }

            if (last) {
                acc_valid_ = false;
                finish_slice(slice, sweep);
            }
        }
    }

   private:
    struct Slot {
        uint32_t seq;
        uint16_t slice;
        uint8_t sweep;
        uint16_t blocks;
        bool last;
        volatile bool full;
    };

    /* Big buffers live in .bss, as upstream keeps them out of the heap. */
    static constexpr size_t kDmaBufferSamples = spec_an::kDmaBufferSamples;
    complex8_t slot_samples_[2][spec_an::kDmaBufferSamples];
    float fft_sin_q_[spec_an::kMaxFft / 4 + 1];
    float pow_acc_[spec_an::kMaxSliceBins];
    spec_an::dsp::CFloat fft_mem_[spec_an::kMaxFft];
    uint8_t waterfall_[spec_an::kWaterfallRows * spec_an::kPoints];

    bool configured_{false};
    spec_an::Shared* shared_{nullptr};
    uint16_t gen_{0};
    size_t log2n_{8};
    size_t n_{256};
    size_t blocks_per_buffer_{8};
    size_t avg_blocks_{1};
    int32_t bin_first_{0};
    size_t bins_per_slice_{0};
    size_t slices_{1};
    spec_an::Window window_{spec_an::Window::BlackmanHarris};

    uint32_t armed_seq_{0};
    uint16_t cap_slice_{0};
    uint8_t cap_sweep_{0};
    size_t settle_left_{0};
    size_t blocks_left_{0};
    bool capturing_{false};
    bool captured_pending_{false};
    uint32_t captured_seq_{0};
    size_t slot_wr_{0};

    std::array<Slot, 2> slots_{};
    size_t slot_rd_{0};
    bool acc_valid_{false};
    uint32_t acc_seq_{0};
    spec_an::dsp::PointDetector detector_{};

   public:
    HostSpecAnEngine() { spec_an::dsp::init_sin_table(fft_sin_q_); }

   private:
    void post_captured() {
        const SpecAnCapturedMessage message{gen_, captured_seq_};
        if (shared_memory.application_queue.push(message)) {
            captured_pending_ = false;
            EventDispatcher::check_fifo_isr();
        }
    }

    void finish_slice(uint16_t slice, uint8_t sweep) {
        static uint32_t slice_count = 0;
        if ((slice_count++ & 0x3f) == 0)
            std::fprintf(stderr, "host_spec_an: slice gen=%u slice=%u sweep=%u\n",
                         (unsigned)gen_, (unsigned)slice, (unsigned)sweep);
        detector_.run(pow_acc_, bins_per_slice_, slice, sweep, shared_->pix[sweep & 1]);

        const bool last = (slice + 1u) >= slices_;
        const SpecAnSliceMessage message{gen_, slice, sweep, last,
                                         detector_.px_begin(), detector_.px_end()};

        /* Upstream retries briefly when the queue is full; on the host a full
         * queue is drained by the UI thread, so retry a few times then drop. */
        for (size_t tries = 0; tries < 50; tries++) {
            if (shared_memory.application_queue.push(message)) break;
            chThdSleepMilliseconds(1);
        }
        EventDispatcher::check_fifo_isr();
    }
};

HostSpecAnEngine g_spec_an;

/* ------------------------------------------------------------------------- */
/* Transmit (Replay) data plane.                                               */
/*                                                                             */
/* The device's replay image (proc_replay) mirrors the capture path in        */
/* reverse: the application's ReplayThread fills a StreamOutput ring and the   */
/* M4 drains it, interpolating the file's IQ up to the SDR sample rate with    */
/* the firmware's own cascade (dsp::replay::...) before pushing complex8_t     */
/* into the TX DMA. The host runs the identical interpolation and hands the    */
/* block to sdr::Device::write(), so a Pluto actually radiates it.            */
/*                                                                             */
/* The ReplayConfig/FIFOData handshake is reproduced too: on ReplayConfig the  */
/* host builds the ring and posts RequestSignalMessage::FillRequest, which is  */
/* what sets the application's ready_signal and lets ReplayThread start.        */
/* ------------------------------------------------------------------------- */

std::unique_ptr<StreamOutput> g_tx_stream{};
replay::Schedule g_tx_schedule{};
dsp::replay::InterpolationCascade g_tx_interp{};
bool g_tx_input_is_c8 = false;
std::array<complex16_t, 512> g_tx_iq{};

void tx_service_schedule(uint32_t source_rate, OversampleRate oversample_rate) {
    g_tx_schedule = replay::for_total(source_rate, toUType(oversample_rate));
    g_tx_interp = dsp::replay::InterpolationCascade{};
    if (!g_tx_schedule.valid()) return;
    g_baseband_fs = g_tx_schedule.output_rate;
    g_tx_interp.configure(g_tx_schedule.total_factor, g_tx_schedule.fir_factor());
}

/* Drain one block of output-rate complex8 IQ from the replay ring, exactly as
 * ReplayProcessor::execute() does. Returns the number of output samples. */
size_t tx_replay_read(int8_t* const out, const size_t out_samples) {
    if (!g_tx_stream || !g_tx_schedule.valid()) {
        std::fill_n(reinterpret_cast<complex8_t*>(out), out_samples, complex8_t{});
        return out_samples;
    }

    auto* c8 = reinterpret_cast<complex8_t*>(out);
    const buffer_c8_t buffer{c8, out_samples, g_baseband_fs};

    const size_t factor = g_tx_schedule.total_factor;
    const size_t source_count = buffer.count / factor;
    if (source_count * factor != buffer.count) {
        std::fill_n(buffer.p, buffer.count, complex8_t{});
        return buffer.count;
    }

    const size_t bytes_to_read = source_count * (g_tx_input_is_c8 ? sizeof(complex8_t) : sizeof(complex16_t));
    if (g_tx_input_is_c8) {
        /* Native signed C8 is already the hardware format; x1 only. */
        const size_t got = g_tx_stream->read(buffer.p, bytes_to_read);
        const size_t complete = got / sizeof(complex8_t);
        std::fill_n(buffer.p + complete, buffer.count - complete, complex8_t{});
    } else {
        /* ReplayProcessor::execute() reads the source in chunks of its iq[]
         * scratch (512 complex16_t) and interpolates each chunk into its slice
         * of the output. A single 512-sample scratch can be far smaller than the
         * whole source span (2048 samples at x1), so chunking is required. */
        bool starved = false;
        size_t total_got = 0;
        for (size_t offset = 0; offset < source_count;) {
            const size_t count = std::min(g_tx_iq.size(), source_count - offset);
            const size_t want = count * sizeof(complex16_t);
            const size_t got = starved ? 0 : g_tx_stream->read(g_tx_iq.data(), want);
            total_got += got;
            starved |= got != want;
            const size_t samples = got / sizeof(complex16_t);
            g_tx_interp.execute_buffer(g_tx_iq.data(), samples,
                                       buffer.p + offset * factor, count * factor);
            offset += count;
        }
        /* Unused for now, but kept so a progress/EOF check can be added without
         * changing the loop shape. */
        (void)total_got;
    }
    return buffer.count;
}

/* ------------------------------------------------------------------------- */
/* Audio receive chain.                                                        */
/*                                                                             */
/* The device runs one baseband image per mode (proc_nfm_audio, proc_am_audio, */
/* proc_wfm_audio); each builds a decimation chain out of the same DSP blocks  */
/* and feeds AudioOutput, which squelches, filters and pushes mono PCM to the  */
/* I2S DMA. The host has no such image, so this plays the whole chain with the */
/* firmware's own blocks - the same decimators, demodulators and biquads - and */
/* hands the resulting PCM to host_audio instead of the codec.                  */
/*                                                                             */
/* The AudioOutput tail is reproduced here (block buffer -> squelch -> HPF ->   */
/* de-emphasis -> int16) because the upstream AudioOutput's fill path depends   */
/* on audio::dma. Only the sink differs.                                        */
/* ------------------------------------------------------------------------- */

enum class AudioMode { None,
                       NFM,
                       AM,
                       WFM,
                       WFMAM };

AudioMode g_audio_mode = AudioMode::None;
uint32_t g_audio_rate = 0;

/* Audio recording ring (proc_*_audio's AudioOutput stream), declared before the
 * tail so AudioTail::on_block can mirror AudioOutput::fill_audio_buffer(). */
std::unique_ptr<StreamInput> g_audio_stream{};

/* Which baseband image the application last started. The device's M4 runs one
 * image at a time and only that image's CaptureConfig handler is live, so this
 * is what decides whether a CaptureConfig belongs to the capture ring or to an
 * audio output ring. It is updated from m4_init() (see hardware_stubs.cpp). */
Image g_image = Image::Unknown;

/* True once set_baseband_rate() has configured the generic RX chain for an
 * Image::Other/Unknown app, so a repeated identical rate is a no-op but a new
 * app (or a different rate) rebuilds it. Cleared by set_image(). */
bool g_generic_rx = false;

/* AudioOutput stand-in: block buffer + squelch + two biquads + int16 sink. */
struct AudioTail {
    BlockDecimator<float, 32> block{1};
    IIRBiquadFilter hpf{};
    IIRBiquadFilter deemph{};
    FMSquelch squelch{};
    uint64_t present_history{0};
    bool present{false};

    void configure(const iir_biquad_config_t& hpf_c,
                   const iir_biquad_config_t& deemph_c = iir_config_passthrough,
                   const float squelch_threshold = 0.0f) {
        hpf.configure(hpf_c);
        deemph.configure(deemph_c);
        squelch.set_threshold(squelch_threshold);
    }

    void write(const buffer_f32_t& audio) {
        block.feed(audio, [this](const buffer_f32_t& b) { on_block(b); });
    }

    void write_s16(const buffer_s16_t& audio) {
        std::array<float, 32> f;
        const size_t n = std::min(audio.count, f.size());
        for (size_t i = 0; i < n; i++) f[i] = audio.p[i] * (1.0f / 32768.0f);
        write({f.data(), n, audio.sampling_rate});
    }

    void on_block(const buffer_f32_t& audio) {
        const bool now = squelch.execute(audio);
        hpf.execute_in_place(audio);
        deemph.execute_in_place(audio);

        present_history = (present_history << 1) | (now ? 1 : 0);
        present = (present_history != 0);

        std::array<int16_t, 32> out;
        for (size_t i = 0; i < audio.count; i++) {
            const int16_t s = present ? (int16_t)__SSAT((int32_t)(audio.p[i] * 32768.0f), 16) : 0;
            out[i] = s;
        }
        if (g_audio_mode != AudioMode::None) {
            host_audio::write_pcm(out.data(), audio.count, g_audio_rate);
        }

        /* Audio file recording: the RecordView's CaptureThread drains this ring
         * exactly as in capture mode (proc_*_audio routes CaptureConfig to
         * AudioOutput's StreamInput). Mirror AudioOutput::fill_audio_buffer(),
         * which writes the post-squelch int16 samples when a stream is set. */
        if (g_audio_stream) {
            g_audio_stream->write(out.data(), audio.count * sizeof(out[0]));
        }
    }
};

AudioTail g_tail{};

/* WFM apt_write equivalent (NoaaApt): AM-magnitude envelope from the real
 * audio before the tail, as AudioOutput::apt_write does. */
float g_apt_prev = 0.0f, g_apt_prev2 = 0.0f;
constexpr float kAptCosTheta = 0.30901699437494742410f;
constexpr float kAptSinTheta = 0.95105651629515357212f;

/* Per-mode decimators. Only the active mode's members hold valid taps; the rest
 * are left configured but idle, exactly as an unused member of a processor. */
dsp::decimate::FIRC8xR16x24FS4Decim4 g_a_dec0{};
dsp::decimate::FIRC16xR16x16Decim2 g_a_audio_dec0{};
dsp::FrequencyTranslatingDecimator32By8 g_a_translating{};
dsp::decimate::FIRAndDecimateComplex g_a_channel{};
dsp::decimate::FIRAndDecimateComplex g_a_dec2{}; /* AM */
dsp::decimate::FIRC16xR16x16Decim2 g_a_wfm_dec1_dec2{};
dsp::decimate::FIRC16xR16x32Decim8 g_a_wfm_dec1_dec8{};
bool g_wfm_dec1_is8 = false;
dsp::decimate::DecimateBy2CIC4Real g_a_wfm_dec1{};
dsp::decimate::DecimateBy2CIC4Real g_a_wfm_dec2{};
dsp::decimate::FIR64AndDecimateBy2Real g_a_wfm_filter{};
dsp::demodulate::FM g_a_demod_fm{};
dsp::demodulate::AM g_a_demod_am{};
dsp::demodulate::SSB g_a_demod_ssb{};
dsp::demodulate::SSB_FM g_a_demod_ssb_fm{};
FeedForwardCompressor g_a_compressor{};

uint8_t g_a_am_modulation = 0; /* AMConfigureMessage::Modulation */
uint8_t g_a_am_squelch = 0;

std::array<complex16_t, 512> g_a_dst{};
const buffer_c16_t g_a_dst_buffer{g_a_dst.data(), g_a_dst.size()};
std::array<int16_t, 512> g_a_work_s16{};
const buffer_s16_t g_a_work_buffer{g_a_work_s16.data(), g_a_work_s16.size()};
std::array<int16_t, 64> g_a_audio_s16{};
const buffer_s16_t g_a_audio16_buffer{g_a_audio_s16.data(), g_a_audio_s16.size()};
std::array<float, 64> g_a_audio_f32{};
const buffer_f32_t g_a_audio32_buffer{g_a_audio_f32.data(), g_a_audio_f32.size()};

/* Audio-mode waterfall: collect 256 * decim complex16 samples and halfband-
 * decimate log2(decim) times down to 256 bins, then reuse the capture FFT.
 * Mirrors FilteredSpectrumCollector: the decimation is a real halfband filter,
 * not sample dropping, so the row is free of aliasing. */
std::array<complex16_t, 256 * 16> g_as_capture{};
std::array<complex16_t, 256 * 16> g_as_stage{};
dsp::decimate::FIRC16xR16x63HalfbandDecim2 g_as_halfband{};
size_t g_audio_spectrum_decim = 1;
size_t g_audio_spectrum_count = 0;

void audio_spectrum_reset() {
    g_audio_spectrum_count = 0;
}

void audio_spectrum_feed(const buffer_c16_t& buffer, size_t decim) {
    if (!g_streaming || buffer.count == 0) return;
    if (decim < 1) decim = 1;
    if (decim > 16) decim = 16;

    const size_t required = g_fft_raw.size() * decim;
    const size_t take = std::min(buffer.count, required - g_audio_spectrum_count);
    std::copy_n(buffer.p, take, g_as_capture.begin() + g_audio_spectrum_count);
    g_audio_spectrum_count += take;
    if (g_audio_spectrum_count != required) return;

    size_t count = required;
    uint32_t rate = buffer.sampling_rate;
    for (size_t d = decim; d > 1; d >>= 1) {
        g_as_halfband.configure(taps_audio_spectrum_halfband.taps);
        const buffer_c16_t in{g_as_capture.data(), count, rate};
        buffer_c16_t out{g_as_stage.data(), count / 2};
        const auto res = g_as_halfband.execute(in, out);
        std::copy_n(res.p, res.count, g_as_capture.begin());
        count = res.count;
        rate = res.sampling_rate;
    }

    std::copy_n(g_as_capture.begin(), count, g_fft_raw.begin());
    g_fft_sampling_rate = rate;
    g_audio_spectrum_count = 0;
    feed_spectrum();
}

void audio_stop() {
    g_audio_mode = AudioMode::None;
    g_audio_stream.reset();
    host_audio::stop_pcm();
    g_apt_prev = g_apt_prev2 = 0.0f;
    g_audio_spectrum_count = 0;
}

void audio_process(const buffer_c8_t& buffer) {
    switch (g_audio_mode) {
        case AudioMode::NFM: {
            const auto decim_0_out = g_a_dec0.execute(buffer, g_a_dst_buffer);
            const auto audio_decim_0_out = g_a_audio_dec0.execute(decim_0_out, g_a_dst_buffer);
            audio_spectrum_feed(audio_decim_0_out, 2);
            const auto decim_1_out = g_a_translating.execute(audio_decim_0_out, g_a_dst_buffer);
            const auto channel_out = g_a_channel.execute(decim_1_out, g_a_dst_buffer);
            auto audio = g_a_demod_fm.execute(channel_out, g_a_audio16_buffer);
            g_tail.write_s16(audio);
            break;
        }

        case AudioMode::AM: {
            const auto decim_0_out = g_a_dec0.execute(buffer, g_a_dst_buffer);
            const auto audio_decim_0_out = g_a_audio_dec0.execute(decim_0_out, g_a_dst_buffer);
            audio_spectrum_feed(audio_decim_0_out, 2);
            const auto decim_1_out = g_a_translating.execute(audio_decim_0_out, g_a_dst_buffer);
            const auto decim_2_out = g_a_dec2.execute(decim_1_out, g_a_dst_buffer);
            const auto channel_out = g_a_channel.execute(decim_2_out, g_a_dst_buffer);

            buffer_f32_t audio = (g_a_am_modulation == (int)AMConfigureMessage::Modulation::SSB)
                                     ? g_a_demod_ssb.execute(channel_out, g_a_audio32_buffer)
                                 : (g_a_am_modulation == (int)AMConfigureMessage::Modulation::SSB_FM)
                                     ? g_a_demod_ssb_fm.execute(channel_out, g_a_audio32_buffer)
                                     : g_a_demod_am.execute(channel_out, g_a_audio32_buffer);

            if (g_a_am_squelch > 0 && channel_out.count > 0) {
                uint64_t sum_mag_sq = 0;
                for (size_t i = 0; i < channel_out.count; i++) {
                    const int32_t re = channel_out.p[i].real();
                    const int32_t im = channel_out.p[i].imag();
                    sum_mag_sq += (uint64_t)re * re + (uint64_t)im * im;
                }
                constexpr float full_scale_mag2 = 32768.0f * 32768.0f;
                const float mean_mag2_norm = (float)sum_mag_sq / ((float)channel_out.count * full_scale_mag2);
                const float level_dbfs = (mean_mag2_norm > 0.0f) ? mag2_to_dbv_norm(mean_mag2_norm) : -200.0f;
                const float threshold_dbfs = -80.0f + (g_a_am_squelch - 1) * (60.0f / 98.0f);
                if (level_dbfs < threshold_dbfs) {
                    for (size_t i = 0; i < audio.count; i++) audio.p[i] = 0.0f;
                }
            }

            g_a_compressor.execute_in_place(audio);
            g_tail.write(audio);
            break;
        }

        case AudioMode::WFM:
        case AudioMode::WFMAM: {
            const auto decim_0_out = g_a_dec0.execute(buffer, g_a_dst_buffer);
            const auto channel = g_wfm_dec1_is8
                                     ? g_a_wfm_dec1_dec8.execute(decim_0_out, g_a_dst_buffer)
                                     : g_a_wfm_dec1_dec2.execute(decim_0_out, g_a_dst_buffer);
            audio_spectrum_feed(channel, 1);

            auto oversampled = g_a_demod_fm.execute(channel, g_a_work_buffer);
            auto audio_4fs = g_a_wfm_dec1.execute(oversampled, g_a_work_buffer);
            auto audio_2fs = g_a_wfm_dec2.execute(audio_4fs, g_a_work_buffer);
            auto audio = g_a_wfm_filter.execute(audio_2fs, g_a_work_buffer);

            if (!g_wfm_dec1_is8) {
                g_tail.write_s16(audio);
            } else {
                /* WFMAM/APT: envelope from the real int16 audio, then the tail. */
                std::array<float, 64> env;
                const size_t n = std::min(audio.count, env.size());
                for (size_t i = 0; i < n; i++) {
                    const float cur = audio.p[i];
                    const float cur2 = cur * cur;
                    const float mag_am = std::sqrt(g_apt_prev2 + cur2 - (2.0f * g_apt_prev * cur * kAptCosTheta)) / kAptSinTheta;
                    env[i] = mag_am * (1.0f / 32768.0f);
                    g_apt_prev = cur;
                    g_apt_prev2 = cur2;
                }
                g_tail.write({env.data(), n, audio.sampling_rate});
            }
            break;
        }

        default:
            break;
    }
}

void audio_configure_nfm(const NBFMConfigureMessage& m) {
    /* These audio chains assume the 3.072 MS/s audio image. When an M4 decoder
     * (weather, subghz, POCSAG, ...) owns the IQ stream, its data plane runs at
     * the rate the app programmed for it and the audio chain is bypassed
     * entirely (see process()). Configuring it here would clobber the shared
     * baseband rate, so the decoder and SDR would disagree about the sample
     * rate - the app then feeds blocks labelled 3.072 MS/s to a decoder
     * configured for the app's real rate, and nothing decodes. */
    if (host_m4::active()) return;

    constexpr uint32_t baseband_fs = 3072000;
    constexpr uint32_t decim_0_output_fs = baseband_fs / 4;
    constexpr uint32_t audio_decim_0_output_fs = decim_0_output_fs / 2;
    constexpr uint32_t channel_filter_input_fs = audio_decim_0_output_fs / 8;

    g_baseband_fs = baseband_fs;
    g_a_dec0.configure(m.decim_0_filter.taps, 33554432,
                       dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
    g_a_audio_dec0.configure(taps_audio_wide_halfband_0.taps);
    g_a_translating.configure(m.decim_1_filter.taps, audio_decim_0_output_fs);
    g_a_channel.configure(m.channel_filter.taps, m.channel_decimation);
    g_a_demod_fm.configure((float)(channel_filter_input_fs / m.channel_decimation), (float)m.deviation);
    g_tail.configure(m.audio_hpf_config, m.audio_deemph_config, (float)m.squelch_level / 100.0f);
    g_audio_rate = 24000;
    g_audio_mode = AudioMode::NFM;
}

void audio_configure_am(const AMConfigureMessage& m) {
    if (host_m4::active()) return; /* see audio_configure_nfm() */

    constexpr uint32_t baseband_fs = 3072000;
    constexpr uint32_t decim_0_output_fs = baseband_fs / 4;
    constexpr uint32_t audio_decim_0_output_fs = decim_0_output_fs / 2;
    constexpr uint32_t decim_1_output_fs = audio_decim_0_output_fs / 8; /* translating /8 */
    constexpr uint32_t decim_2_output_fs = decim_1_output_fs / 4;

    g_baseband_fs = baseband_fs;
    g_a_dec0.configure(m.decim_0_filter.taps, 33554432,
                       dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
    g_a_audio_dec0.configure(taps_audio_wide_halfband_0.taps);
    g_a_translating.configure(m.decim_1_filter.taps, audio_decim_0_output_fs);
    g_a_dec2.configure(m.decim_2_filter.taps, 4);
    g_a_channel.configure(m.channel_filter.taps, 1);
    g_a_am_modulation = (uint8_t)m.modulation;
    g_a_am_squelch = m.squelch_level;
    (void)decim_2_output_fs;
    g_tail.configure(m.audio_hpf_lpf_config);
    g_audio_rate = 12000;
    g_audio_mode = AudioMode::AM;
}
void audio_configure_wfm(const WFMConfigureMessage& m) {
    if (host_m4::active()) return; /* see audio_configure_nfm() */

    constexpr uint32_t baseband_fs = 3072000;
    constexpr uint32_t decim_0_output_fs = baseband_fs / 4;

    g_baseband_fs = baseband_fs;
    g_a_dec0.configure(m.decim_0_filter.taps, dsp::decimate::c8_to_c32_sat_scalar,
                       dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
    g_a_wfm_dec1_dec2.configure(m.decim_1_filter.taps);
    g_wfm_dec1_is8 = false;
    g_a_demod_fm.configure((float)(decim_0_output_fs / 2), (float)m.deviation);
    g_a_wfm_filter.configure(m.audio_filter.taps);
    g_tail.configure(m.audio_hpf_config, m.audio_deemph_config);
    g_audio_rate = 48000;
    g_audio_mode = AudioMode::WFM;
}

void audio_configure_wfmam(const WFMAMConfigureMessage& m) {
    if (host_m4::active()) return; /* see audio_configure_nfm() */

    constexpr uint32_t baseband_fs = 3072000;
    constexpr uint32_t decim_0_output_fs = baseband_fs / 4;

    g_baseband_fs = baseband_fs;
    g_a_dec0.configure(m.decim_0_filter.taps, dsp::decimate::c8_to_c32_sat_scalar,
                       dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
    g_a_wfm_dec1_dec8.configure(m.decim_1_filter.taps);
    g_wfm_dec1_is8 = true;
    g_a_demod_fm.configure((float)(decim_0_output_fs / 8), (float)m.deviation);
    g_a_wfm_filter.configure(m.audio_filter.taps);
    g_apt_prev = g_apt_prev2 = 0.0f;
    g_tail.configure(m.audio_hpf_config, m.audio_deemph_config);
    g_audio_rate = 12000;
    g_audio_mode = AudioMode::WFMAM;
}

void configure(uint32_t sample_rate, OversampleRate oversample_rate) {
    g_sample_rate = sample_rate;
    g_oversample = toUType(oversample_rate);
    g_baseband_fs = sample_rate * g_oversample;

    g_filter_low_f = (int32_t)(taps_200k_decim_1.low_frequency_normalized * sample_rate);
    g_filter_high_f = (int32_t)(taps_200k_decim_1.high_frequency_normalized * sample_rate);
    g_filter_transition = (int32_t)(taps_200k_decim_1.transition_normalized * sample_rate);

    /* CaptureProcessor::sample_rate_config(): pace waterfall frames at a fixed
     * rate, with the x8 oversample correction and the high-rate speed-up. */
    const auto oversample_correction = g_oversample / 8.0;
    if (oversample_correction > 0.0) {
        g_spectrum_interval_samples = (size_t)(sample_rate / (kSpectrumRateHz * oversample_correction));
    }
    if (sample_rate >= 1'500'000) {
        g_spectrum_interval_samples /= (sample_rate / 750'000);
    }

    switch (oversample_rate) {
        case OversampleRate::x4:
            g_d0_dec4.configure(taps_200k_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar, dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
            g_stage = 0;
            g_stage1 = 0;
            break;

        case OversampleRate::x8:
            if (sample_rate < 600'000) {
                g_d0_dec4.configure(taps_200k_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar, dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
                g_stage = 0;
                g_d1_dec2.configure(taps_200k_decim_1.taps);
                g_stage1 = 1;
            } else {
                g_d0_dec8.configure(taps_180k_wfm_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar, dsp::decimate::FIRC8xR16x24FS4Decim8::Shift::Down);
                g_stage = 1;
                g_stage1 = 0;
            }
            break;

        case OversampleRate::x16:
            g_d0_dec8.configure(taps_200k_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar, dsp::decimate::FIRC8xR16x24FS4Decim8::Shift::Down);
            g_stage = 1;
            g_d1_dec2.configure(taps_200k_decim_1.taps);
            g_stage1 = 1;
            break;

        case OversampleRate::x32:
            g_d0_dec4.configure(taps_200k_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar, dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
            g_stage = 0;
            g_d1_dec8.configure(taps_16k0_decim_1.taps);
            g_stage1 = 2;
            break;

        case OversampleRate::x64:
            g_d0_dec8.configure(taps_200k_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar, dsp::decimate::FIRC8xR16x24FS4Decim8::Shift::Down);
            g_stage = 1;
            g_d1_dec8.configure(taps_16k0_decim_1.taps);
            g_stage1 = 2;
            break;

        default:
            break;
    }

    spectrum_reset();
}

void emit(const buffer_c16_t& out) {
    if (g_stream && out.count) {
        g_stream->write(out.p, sizeof(*out.p) * out.count);
    }

    if (g_streaming && out.count) {
        /* Port of CaptureProcessor::execute(): advance the sample counter and,
         * once the update interval has elapsed, grant a single frame's worth of
         * credit. Only when a frame is not already pending and a full 256-sample
         * grant is available do we start collecting - then feed_spectrum() takes
         * at most those 256 samples, so the FIFO sees ~50 rows/s, not one row per
         * decimated burst. */
        g_spectrum_samples += out.count;
        if (g_spectrum_samples >= g_spectrum_interval_samples) {
            g_spectrum_samples = std::min(g_spectrum_samples - g_spectrum_interval_samples, g_spectrum_interval_samples);
            g_spectrum_capture_credit = (uint16_t)std::min<size_t>(
                256, g_spectrum_capture_credit + std::min<size_t>(out.count, 256));
        }
        if (!g_spectrum_capture_remaining && g_spectrum_capture_credit == 256) {
            g_spectrum_capture_credit = 0;
            g_spectrum_capture_remaining = 256;
        }
        feed_channel_spectrum(out);
    }
}

void process(const complex8_t* samples, size_t count) {
    const buffer_c8_t buffer{samples, count, g_baseband_fs};

    /* A registered M4 decoder (weather, subghz, POCSAG, ADS-B, ...) owns the
     * raw IQ stream: hand it the samples untouched at the baseband rate, the
     * same blocks the device's SGPIO DMA would have delivered. Its own
     * execute() runs the decimation, demodulation, protocol decoding and (for
     * the apps that stream one) the waterfall, so the host's generic chain must
     * stay out of the way to avoid a second, competing spectrum feed. */
    if (host_m4::active()) {
        host_m4::feed(reinterpret_cast<const int8_t*>(samples), count, g_baseband_fs);
        return;
    }

    /* SpecAn sweep mode owns the raw DMA stream: the M0 retunes per slice and
     * the engine FFTs the requested buffers. Decimation below would destroy
     * the wide span the sweep is measuring. */
    if (g_spec_an.active()) {
        g_spec_an.execute(buffer);
        g_spec_an.process_pending();
        return;
    }

    if (g_audio_mode != AudioMode::None) {
        audio_process(buffer);
        return;
    }

    buffer_c16_t stage0 = (g_stage == 0)
                              ? g_d0_dec4.execute(buffer, g_dst_buffer)
                              : g_d0_dec8.execute(buffer, g_dst_buffer);

    if (g_stage1 == 1) {
        emit(g_d1_dec2.execute(stage0, g_dst_buffer));
    } else if (g_stage1 == 2) {
        emit(g_d1_dec8.execute(stage0, g_dst_buffer));
    } else {
        emit(stage0);
    }
}

/* Map the firmware's HackRF gain stages onto the backend tuner's manual-gain
 * span. On the device LNA (0..40 dB) and VGA (0..62 dB) are two analogue stages
 * and the front-end RF amp adds about 14 dB, so the full range is 116 dB. An
 * RTL tuner spans far less (e.g. 0..49.6 dB), so the sum is mapped
 * proportionally: the LNA/VGA sliders keep their relative effect, and the
 * defaults (LNA 32 + VGA 32) land near the middle of the tuner's range rather
 * than pinned at a rail. */
constexpr int kHackRfMaxGainDb = 40 + 62 + 14;

double map_gain_to_backend(const int lna_db, const int vga_db, const bool amp,
                           const double range_min, const double range_max) {
    int target = lna_db + vga_db + (amp ? 14 : 0);
    if (target < 0) target = 0;
    if (target > kHackRfMaxGainDb) target = kHackRfMaxGainDb;
    if (range_max <= range_min) return static_cast<double>(target);
    const double frac = static_cast<double>(target) / kHackRfMaxGainDb;
    return range_min + frac * (range_max - range_min);
}

void producer_thread() {
    std::unique_ptr<sdr::Device> device;
    double applied_freq = -1.0;
    uint32_t applied_rate = 0;
    double applied_gain = -1.0;
    bool applied_auto_gain = false;
    int applied_bias = -1; /* -1 = not yet applied */
    double gain_min = 0.0;
    double gain_max = 0.0;
    std::vector<int8_t> raw(kBufferSamples * 2);
    std::vector<int8_t> tx_raw(kBufferSamples * 2);
    /* Consecutive zero-length reads. A running device should always return data,
     * so a short run of zeros means the stream died (unplugged, powered off,
     * driver error). After this many we drop the device and reopen, so a
     * reattached SDR is picked up again instead of leaving RX/TX dead. */
    constexpr int kReadFailLimit = 3;
    int read_failures = 0;
    bool tx_warned = false;

    while (g_running.load()) {
        uint32_t rate;
        uint64_t freq;
        int lna_db;
        int vga_db;
        bool amp;
        bool bias;
        bool tx;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            rate = g_baseband_fs;
            freq = host_radio::tuning_frequency();
            lna_db = host_radio::lna_gain_db();
            vga_db = host_radio::vga_gain_db();
            amp = host_radio::rf_amp();
            bias = host_radio::antenna_bias();
            tx = host_radio::transmitting();
        }

        if (!device) {
            if (rate == 0 || freq == 0) {
                /* No RX stream is configured yet (e.g. the Replay/Remote image,
                 * which never sends a SampleRateConfig). There is nothing to
                 * receive, but the app's waterfall may already be streaming, so
                 * keep it fed instead of leaving it black. */
                feed_floor_if_due();
                chThdSleepMicroseconds(2000);
                continue;
            }
            device = sdr::open_preferred();
            if (!device) {
                /* No device (or open failed): keep polling so plugging one in
                 * later is detected without restarting the process. */
                feed_floor_if_due();
                chThdSleepMicroseconds(20000);
                continue;
            }
            device->gain_range(gain_min, gain_max);
            std::fprintf(stderr,
                         "host_baseband: SDR opened, tuner gain range %.1f..%.1f dB\n",
                         gain_min, gain_max);
            applied_freq = -1.0;
            applied_rate = 0;
            applied_gain = -1.0;
            applied_auto_gain = false;
            applied_bias = -1;
            read_failures = 0;
        }

        /* Transmit: the application has switched the radio to Transmit and the
         * backend can radiate. Tune the independent TX LO, set TX gain, and
         * stream interpolated IQ from the replay ring. */
        if (tx && device->tx_supported()) {
            if ((double)freq != applied_freq) {
                device->set_tx_frequency((double)freq);
                applied_freq = (double)freq;
            }
            if (rate != 0 && rate != applied_rate) {
                device->set_sample_rate((double)rate);
                applied_rate = rate;
            }
            const int tx_db = host_radio::tx_gain_db();
            const double tx_target = (double)tx_db;
            if (tx_target != applied_gain) {
                device->set_tx_gain(tx_target);
                applied_gain = tx_target;
                std::fprintf(stderr, "host_baseband: TX gain -> %.1f dB\n", tx_target);
            }

            std::lock_guard<std::mutex> lk(g_mutex);
            const size_t n = tx_replay_read(tx_raw.data(), kBufferSamples);
            device->write(tx_raw.data(), n);
            /* TX has no RX stream to analyse: keep the waterfall alive. */
            feed_floor_if_due();
            continue;
        }

        /* Transmit requested but the backend cannot radiate (e.g. an RTL-SDR).
         * Report it once and idle instead of running the receive chain, which
         * would feed the app's TX-shaped state through RX decoders. */
        if (tx) {
            if (!tx_warned) {
                std::fprintf(stderr,
                             "host_baseband: transmit requested but this SDR backend "
                             "has no TX path; replay will not radiate\n");
                tx_warned = true;
            }
            /* No TX path and no RX stream: still keep the waterfall alive. */
            feed_floor_if_due();
            chThdSleepMilliseconds(20);
            continue;
        }
        tx_warned = false;

        if ((double)freq != applied_freq) {
            device->set_center_freq((double)freq);
            applied_freq = (double)freq;
            std::fprintf(stderr, "host_baseband: center freq -> %llu Hz (applied %.0f)\n",
                         (unsigned long long)freq, device->center_freq());
        }
        if (rate != 0 && rate != applied_rate) {
            device->set_sample_rate((double)rate);
            applied_rate = rate;
        }

        /* Manual gain: map the firmware's LNA+VGA(+RF amp) onto the tuner's own
         * span. This makes the apps' LNA/VGA sliders and the RF-amp toggle real
         * instead of the fixed 30 dB the bring-up used. A backend that has no
         * real gain span (RTL) runs its own AGC instead, once. */
        if (device->prefers_auto_gain()) {
            if (!applied_auto_gain) {
                device->set_gain_mode(false); /* false = automatic */
                std::fprintf(stderr,
                             "host_baseband: tuner AGC enabled (backend has no "
                             "analogue LNA/VGA span)\n");
                applied_auto_gain = true;
            }
        } else {
            const double target_gain = map_gain_to_backend(lna_db, vga_db, amp, gain_min, gain_max);
            if (target_gain != applied_gain) {
                device->set_gain_mode(true);
                device->set_gain(target_gain);
                std::fprintf(stderr,
                             "host_baseband: gain LNA=%d VGA=%d amp=%d -> %.1f dB\n",
                             lna_db, vga_db, amp ? 1 : 0, target_gain);
                applied_gain = target_gain;
            }
        }

        /* Antenna bias tee: the firmware requests it for TX and for apps that
         * power an active antenna; forward it to the backend where supported. */
        const int bias_flag = bias ? 1 : 0;
        if (bias_flag != applied_bias) {
            device->set_bias_tee(bias);
            applied_bias = bias_flag;
            std::fprintf(stderr, "host_baseband: bias tee %s\n", bias ? "on" : "off");
        }

        const size_t got = device->read(raw.data(), kBufferSamples);
        if (got == 0) {
            if (++read_failures >= kReadFailLimit) {                /* The device is gone. Close and forget it; the next loop pass
                 * reopens whatever is attached now. */
                device->close();
                device.reset();
                read_failures = 0;
                chThdSleepMilliseconds(500); /* let USB re-enumerate */
            } else {
                chThdSleepMilliseconds(50);
            }
            continue;
        }
        read_failures = 0;

        std::lock_guard<std::mutex> lk(g_mutex);
        process(reinterpret_cast<const complex8_t*>(raw.data()), got);
    }

    if (device) device->close();
}

}  // namespace

/* Generic receive apps (ADS-B, AIS, EPIRB, 2-Tone, POCSAG, ...) never send a
 * SampleRateConfig: they configure the radio through ReceiverModel::enable(),
 * whose update_sampling_rate() calls radio::set_baseband_rate() ->
 * ClockManager::set_sampling_frequency(). On the device that programs the clock
 * generator and the M4 image already knows its own rate. On the host the clock
 * call was a no-op, so g_baseband_fs stayed 0, the producer never opened the SDR
 * and the app's waterfall sat on a dead black band.
 *
 * Learn the rate here and bring up a minimal decimate + spectrum chain. Only for
 * images that own no other data plane (Image::Other/Unknown): capture, audio,
 * replay and spec-an configure g_baseband_fs themselves and must not be
 * clobbered. */
void set_baseband_rate(const uint32_t rate) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_image != Image::Other && g_image != Image::Unknown) return;
    if (g_spec_an.active()) return; /* the sweep owns the SDR rate */
    if (rate == 0) return;
    if (g_generic_rx && g_baseband_fs == rate) return;

    g_generic_rx = true;
    g_baseband_fs = rate;
    g_sample_rate = rate / 4;
    g_oversample = 4;
    std::fprintf(stderr, "host_baseband: generic RX rate=%u image=%d\n", rate, (int)g_image);

    /* The same 1:4 stage CaptureProcessor uses for OversampleRate::x4. */
    g_d0_dec4.configure(taps_200k_decim_0.taps, dsp::decimate::c8_to_c32_sat_scalar,
                        dsp::decimate::FIRC8xR16x24FS4Decim4::Shift::Down);
    g_stage = 0;
    g_stage1 = 0;

    g_filter_low_f = (int32_t)(taps_200k_decim_1.low_frequency_normalized * g_sample_rate);
    g_filter_high_f = (int32_t)(taps_200k_decim_1.high_frequency_normalized * g_sample_rate);
    g_filter_transition = (int32_t)(taps_200k_decim_1.transition_normalized * g_sample_rate);

    /* ~50 rows/s, as the firmware's spectrum collector paces itself. */
    g_spectrum_interval_samples = g_sample_rate / (size_t)kSpectrumRateHz;
    if (g_spectrum_interval_samples == 0) g_spectrum_interval_samples = 1;

    spectrum_reset();
}

void handle_message(const Message* message) {
    if (!message) return;

    switch (message->id) {
        case Message::ID::SampleRateConfig: {
            const auto& m = *reinterpret_cast<const SampleRateConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_image == Image::Replay) {
                tx_service_schedule(m.sample_rate, m.oversample_rate);
            } else {
                configure(m.sample_rate, m.oversample_rate);
            }
            break;
        }

        case Message::ID::ReplayConfig: {
            const auto& m = *reinterpret_cast<const ReplayConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            if (m.config) {
                g_tx_stream = std::make_unique<StreamOutput>(m.config);
                g_tx_input_is_c8 = m.config->input_is_c8;
                /* Tell the application its buffers and FIFO pointers are ready,
                 * exactly as proc_replay/proc_audiotx do: this is what sets the
                 * ReplayThread's ready_signal so it starts prefilling. */
                const RequestSignalMessage signal{RequestSignalMessage::Signal::FillRequest};
                shared_memory.application_queue.push(signal);
                EventDispatcher::check_fifo_isr();
            } else {
                g_tx_stream.reset();
            }
            break;
        }

        case Message::ID::FIFOData: {
            /* The application has prefilled the ring; transmission may start.
             * Nothing to configure here - the producer polls g_image and the ring
             * directly - but consuming the message keeps the handshake honest. */
            std::lock_guard<std::mutex> lk(g_mutex);
            break;
        }

        case Message::ID::CaptureConfig: {
            const auto& m = *reinterpret_cast<const CaptureConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            if (m.config) {
                auto stream = std::make_unique<StreamInput>(m.config);
                /* Only the audio images (proc_*_audio) route CaptureConfig to
                 * their AudioOutput stream; every other image - capture above
                 * all - routes it to the IQ ring the RecordView drains. Keying
                 * off the running image, not leftover audio state, is what makes
                 * a capture recording work after the Audio app has been used. */
                if (g_image == Image::Audio) {
                    g_audio_stream = std::move(stream);
                } else {
                    g_stream = std::move(stream);
                }
            } else {
                g_audio_stream.reset();
                g_stream.reset();
            }
            break;
        }

        case Message::ID::NBFMConfigure: {
            const auto& m = *reinterpret_cast<const NBFMConfigureMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            audio_configure_nfm(m);
            audio_spectrum_reset();
            break;
        }

        case Message::ID::AMConfigure: {
            const auto& m = *reinterpret_cast<const AMConfigureMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            audio_configure_am(m);
            audio_spectrum_reset();
            break;
        }

        case Message::ID::WFMConfigure: {
            const auto& m = *reinterpret_cast<const WFMConfigureMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            audio_configure_wfm(m);
            audio_spectrum_reset();
            break;
        }

        case Message::ID::WFMAMConfigure: {
            const auto& m = *reinterpret_cast<const WFMAMConfigureMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            audio_configure_wfmam(m);
            audio_spectrum_reset();
            break;
        }

        case Message::ID::AudioDDCConfig: {
            const auto& m = *reinterpret_cast<const AudioDDCConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_audio_mode == AudioMode::NFM || g_audio_mode == AudioMode::AM) {
                g_a_translating.set_frequency(m.frequency);
            }
            break;
        }

        case Message::ID::Shutdown: {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_spec_an.stop();
            /* Stop the audio sink, but leave the FIFO rings in place. On the
             * device the M4 tearing down an image is safe because the M0 side
             * joins its CaptureThread/ReplayThread first; on the host the
             * application thread may still be mid-write when this arrives, and
             * destroying a ring it holds a pointer into is the "window freezes /
             * segfault on exit" crash (FIFO<StreamBuffer*>::in <- put <-
             * CaptureThread::run). The rings are replaced wholesale on the next
             * CaptureConfig/ReplayConfig, by which point the old thread is gone. */
            g_audio_mode = AudioMode::None;
            host_audio::stop_pcm();
            break;
        }

        case Message::ID::SpectrumStreamingConfig: {
            const auto& m = *reinterpret_cast<const SpectrumStreamingConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            std::fprintf(stderr, "host_baseband: SpectrumStreaming mode=%d\n", (int)m.mode);
            if (m.mode == SpectrumStreamingConfigMessage::Mode::Running) {
                g_streaming = true;
                /* Hand the UI the FIFO it should drain for each row. */
                const ChannelSpectrumConfigMessage cfg{&g_spectrum_fifo};
                shared_memory.application_queue.push(cfg);
                EventDispatcher::check_fifo_isr();
            } else {
                g_streaming = false;
                g_spectrum_fifo.reset_in();
            }
            break;
        }

        case Message::ID::SpecAnConfig: {
            const auto& m = *reinterpret_cast<const SpecAnConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            /* The sweep owns the SDR rate; SampleRateConfig carries the same
             * rate, but set it here too so the engine's buffers are labelled
             * with the rate the plan was computed for. */
            g_baseband_fs = m.sampling_rate;
            g_spec_an.configure(m);
            break;
        }

        case Message::ID::WidebandSpectrumConfig: {
            const auto& m = *reinterpret_cast<const WidebandSpectrumConfigMessage*>(message);
            std::lock_guard<std::mutex> lk(g_mutex);
            /* Leaves sweep mode; the classic wideband path (below) resumes. */
            g_spec_an.stop();
            g_baseband_fs = m.sampling_rate;
            break;
        }

        case Message::ID::UpdateSpectrum: {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_spec_an.process_pending();
            break;
        }

        default:
            break;
    }
}

void start() {
    if (g_started) return;
    g_started = true;
    g_running = true;
    g_thread = std::thread(producer_thread);
    /* The process exits via exit(); the UI thread is torn down there, but this
     * std::thread is not. Register an atexit handler (runs before static
     * destructors, while the thread is still joinable) to stop and join it, so
     * ~thread() never sees a joinable thread and calls std::terminate. */
    std::atexit([] { shutdown(); });
}

void set_image(Image image) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_image = image;
    g_generic_rx = false;
    /* A new image owns the whole baseband state. The previous image's SpecAn
     * sweep (if any) must stop, or its Shared pointer dangles when the app
     * that owned it is destroyed. */
    g_spec_an.stop();
    /* A new image owns the whole baseband state. The previous image's audio
     * configuration (mode, tail, PCM sink) must not leak into it: a capture
     * image that inherited AudioMode::AM would run the audio chain and leave the
     * capture ring unfed forever. */
    if (image != Image::Audio) {
        audio_stop();
    }
    /* Leaving the transmit image drops its replay ring and interpolation state;
     * the next ReplayConfig rebuilds them. */
    if (image != Image::Replay) {
        g_tx_stream.reset();
        g_tx_interp = dsp::replay::InterpolationCascade{};
        g_tx_schedule = replay::Schedule{};
    }
}

void shutdown() {
    if (!g_started) return;
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
    g_started = false;
    g_stream.reset();
    g_tx_stream.reset();
}

}  // namespace host_baseband

/* M4 doorbell: assert_event() in the ported stream rings lands here. Runs the
 * same wake-up the device's M4Core_IRQHandler performed on the M0 side. */
extern "C" void host_baseband_m4_doorbell() {
    BufferExchange::handle_isr();
    EventDispatcher::check_fifo_isr();
}

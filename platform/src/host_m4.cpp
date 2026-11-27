/*
 * Host baseband core: the firmware's real M4 processors, not reimplementations.
 *
 * See platform/include/host_m4.hpp for the rationale. This translation unit is
 * the whole mechanism:
 *
 *   - a small IQ block ring between host_baseband's SDR producer thread and the
 *     image's BasebandThread (the M4 DMA stand-in),
 *   - a factory that maps an image tag to the firmware's own BasebandProcessor
 *     subclass (proc_*.hpp), and
 *   - host implementations of the M4-only support classes the firmware
 *     processors depend on (BasebandThread, RSSIThread, audio::dma, rf::rssi).
 *     Their device versions drive SGPIO/GPDMA/I2S0 hardware that does not exist
 *     here; the host versions bridge to the desktop SDR and audio sink.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "host_m4.hpp"

#include "host_audio.hpp"

#include "dsp_types.hpp"
#include "message.hpp"
#include "portapack_shared_memory.hpp"
#include "spi_image.hpp"

#include "baseband.hpp"
#include "baseband_thread.hpp"
#include "rssi_thread.hpp"
#include "audio_dma.hpp"
#include "rssi.hpp"
#include "rssi_dma.hpp"

#include "ch.h"

/* Take the host shadow of the firmware's M4 event dispatcher before the proc
 * headers below pull in their own (baseband/event_m4.hpp): platform/include is
 * searched before firmware/baseband, so this resolves to the shadow and defines
 * its guard. See platform/include/event_m4.hpp. */
#include "event_m4.hpp"

/* The firmware's own per-image receive processors, compiled into the desktop
 * binary and instantiated by image tag. Every one of these is upstream,
 * unmodified. */
#include "proc_weather.hpp"
#include "proc_subghzd.hpp"
#include "proc_subghz_chat.hpp"
#include "proc_pocsag2.hpp"
#include "proc_flex.hpp"
#include "proc_adsbrx.hpp"
#include "proc_ais.hpp"
#include "proc_aprsrx.hpp"
#include "proc_sonde.hpp"
#include "proc_tpms.hpp"
#include "proc_ert.hpp"
#include "proc_acars.hpp"
#include "proc_afskrx.hpp"
#include "proc_rds_rx.hpp"
#include "proc_wmbus.hpp"
#include "proc_vdl2.hpp"
#include "proc_tetra.hpp"
#include "proc_morse.hpp"
#include "proc_rtty_rx.hpp"
#include "proc_tonedetect.hpp"
#include "proc_epirb.hpp"
#include "proc_noaaapt_rx.hpp"
#include "proc_wefaxrx.hpp"
#include "proc_protoview.hpp"
#include "proc_btlerx.hpp"
#include "proc_nrfrx.hpp"
#include "proc_fsk_rx.hpp"
#include "proc_ook.hpp"
#include "proc_subcar.hpp"
#include "proc_signal_hunter.hpp"
#include "proc_ft8_rx.hpp"
#include "proc_sstvrx.hpp"
#include "proc_vor_rx.hpp"
#include "proc_time_sink.hpp"
#include "proc_am_tv.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>

namespace host_m4 {

/* ------------------------------------------------------------------------- */
/* IQ block ring: host_baseband (producer) -> image BasebandThread (consumer). */
/*                                                                             */
/* On the device the SGPIO/GPDMA pair hands the M4 2048-sample complex8_t      */
/* transfers. host_baseband pulls the same blocks off the SDR; the ring        */
/* decouples its read cadence from the processor's execute() cadence.          */
/* ------------------------------------------------------------------------- */
constexpr size_t kMaxBlockSamples = 4096;

namespace {
constexpr size_t kSlots = 8;

struct Slot {
    std::array<int8_t, kMaxBlockSamples * 2> data{}; /* interleaved I,Q */
    size_t samples = 0;
    uint32_t rate = 0;
};

std::mutex g_ring_mutex;
std::condition_variable g_ring_cv;
std::array<Slot, kSlots> g_ring{};
size_t g_ring_head = 0;
size_t g_ring_tail = 0;
size_t g_ring_count = 0;

std::mutex g_image_mutex;
std::unique_ptr<BasebandProcessor> g_processor{};
std::atomic<bool> g_active{false};

void ring_reset() {
    std::lock_guard<std::mutex> lk(g_ring_mutex);
    g_ring_head = g_ring_tail = g_ring_count = 0;
}

std::unique_ptr<BasebandProcessor> make_processor(
    const portapack::spi_flash::image_tag_t& t) {
#define HOST_M4_DECODER(tag, cls)                   \
    if (t == portapack::spi_flash::image_tag_##tag) \
        return std::make_unique<cls>();
    HOST_M4_DECODER(weather, WeatherProcessor)
    HOST_M4_DECODER(subghzd, SubGhzDProcessor)
    HOST_M4_DECODER(subghz_chat, SubGhzChatProcessor)
    HOST_M4_DECODER(pocsag2, POCSAGProcessor)
    HOST_M4_DECODER(flex, FlexProcessor)
    HOST_M4_DECODER(adsb_rx, ADSBRXProcessor)
    HOST_M4_DECODER(ais, AISProcessor)
    HOST_M4_DECODER(aprs_rx, APRSRxProcessor)
    HOST_M4_DECODER(sonde, SondeProcessor)
    HOST_M4_DECODER(tpms, TPMSProcessor)
    HOST_M4_DECODER(ert, ERTProcessor)
    HOST_M4_DECODER(acars, ACARSProcessor)
    HOST_M4_DECODER(afsk_rx, AFSKRxProcessor)
    HOST_M4_DECODER(rds_rx, RDSProcessor)
    HOST_M4_DECODER(wmbus_rx, WMBusProcessor)
    HOST_M4_DECODER(vdl2_rx, VDL2Processor)
    HOST_M4_DECODER(tetrarx, TetraProcessor)
    HOST_M4_DECODER(morse, MorseProcessor)
    HOST_M4_DECODER(rttyrx, RTTYRxProcessor)
    HOST_M4_DECODER(tonedetect, ToneDetectProcessor)
    HOST_M4_DECODER(epirb_rx, EPIRBProcessor)
    HOST_M4_DECODER(noaaapt_rx, NoaaAptRx)
    HOST_M4_DECODER(wefaxrx, WeFaxRx)
    HOST_M4_DECODER(protoview, ProtoViewProcessor)
    HOST_M4_DECODER(btle_rx, BTLERxProcessor)
    HOST_M4_DECODER(nrf_rx, NRFRxProcessor)
    HOST_M4_DECODER(fskrx, FSKRxProcessor)
    HOST_M4_DECODER(ook, OOKProcessor)
    HOST_M4_DECODER(subcar, SubCarProcessor)
    HOST_M4_DECODER(signal_hunter, SignalHunterProcessor)
    HOST_M4_DECODER(ft8_rx, FT8RxProcessor)
    HOST_M4_DECODER(sstv_rx, SSTVRXProcessor)
    HOST_M4_DECODER(vor_rx, VorRx)
    HOST_M4_DECODER(time_sink, TimeSinkProcessor)
    HOST_M4_DECODER(am_tv, WidebandFMAudio)
#undef HOST_M4_DECODER
    return nullptr;
}
}  // namespace

/* Called by the image's BasebandThread: block until host_baseband produced a
 * block, then copy it out. Returns the byte count (0 on a stop request). */
size_t wait_block(int8_t* out, size_t out_bytes, size_t* samples, uint32_t* rate) {
    std::unique_lock<std::mutex> lk(g_ring_mutex);
    g_ring_cv.wait(lk, [] { return g_ring_count != 0 || !g_active.load(); });
    if (g_ring_count == 0) return 0;
    Slot& s = g_ring[g_ring_head];
    size_t bytes = s.samples * 2;
    if (bytes > out_bytes) bytes = out_bytes;
    std::memcpy(out, s.data.data(), bytes);
    *samples = bytes / 2;
    *rate = s.rate;
    g_ring_head = (g_ring_head + 1) % kSlots;
    g_ring_count--;
    return bytes;
}

/* The M4 asserted the doorbell after producing a message; the M0 side runs
 * check_fifo_isr(). The processor pushes straight into
 * shared_memory.application_queue, so ring the same bell to wake the UI. */
void after_execute() {
    host_baseband_m4_doorbell();
}

void start_image(const portapack::spi_flash::image_tag_t& tag) {
    stop_image();
    ring_reset();
    std::unique_ptr<BasebandProcessor> proc = make_processor(tag);
    {
        std::lock_guard<std::mutex> lk(g_image_mutex);
        g_processor = std::move(proc);
    }
    /* The processor's BasebandThread member has already auto-started and is
     * waiting for blocks; make them visible only now, once the whole object is
     * constructed. */
    g_active = (g_processor != nullptr);
}

void stop_image() {
    g_active = false;
    g_ring_cv.notify_all();
    std::unique_ptr<BasebandProcessor> doomed;
    {
        std::lock_guard<std::mutex> lk(g_image_mutex);
        doomed = std::move(g_processor);
    }
    /* ~BasebandThread (a member of the processor) terminates and joins its
     * thread before the processor's own members are destroyed. */
    doomed.reset();
}

bool active() {
    return g_active.load();
}

void on_message(const Message* message) {
    std::lock_guard<std::mutex> lk(g_image_mutex);
    if (message)
        std::fprintf(stderr, "host_m4: on_message id=%d processor=%d\n",
                     (int)message->id, g_processor ? 1 : 0);
    if (g_processor) g_processor->on_message(message);
}

void feed(const int8_t* samples, size_t count, uint32_t rate) {
    if (!g_active.load() || count == 0) return;
    if (count > kMaxBlockSamples) count = kMaxBlockSamples;
    std::lock_guard<std::mutex> lk(g_ring_mutex);
    if (g_ring_count == kSlots) {
        /* Consumer stalled: drop the oldest block rather than block the SDR. */
        g_ring_head = (g_ring_head + 1) % kSlots;
        g_ring_count--;
    }
    Slot& s = g_ring[g_ring_tail];
    std::memcpy(s.data.data(), samples, count * 2);
    s.samples = count;
    s.rate = rate;
    g_ring_tail = (g_ring_tail + 1) % kSlots;
    g_ring_count++;
    g_ring_cv.notify_one();
}

}  // namespace host_m4

/* ------------------------------------------------------------------------- */
/* Host BasebandThread: the M4 DMA consumer.                                    */
/*                                                                             */
/* The device version drives SGPIO/GPDMA; here it pulls IQ blocks off the ring  */
/* and calls the processor's execute(), which is the only thing the processors  */
/* care about.                                                                  */
/* ------------------------------------------------------------------------- */
Thread* BasebandThread::thread = nullptr;

BasebandThread::BasebandThread(
    uint32_t sampling_rate,
    BasebandProcessor* const baseband_processor,
    baseband::Direction direction,
    bool auto_start,
    tprio_t priority)
    : baseband_processor_{baseband_processor},
      direction_{direction},
      sampling_rate_{sampling_rate},
      priority_{priority} {
    if (auto_start) start();
}

BasebandThread::~BasebandThread() {
    if (thread) {
        chThdTerminate(thread);
        chThdWait(thread);
        thread = nullptr;
    }
}

void BasebandThread::start() {
    if (!thread) {
        thread = chThdCreateFromHeap(nullptr, 8192, priority_, ThreadBase::fn, this);
    }
}

void BasebandThread::set_sampling_rate(uint32_t new_sampling_rate) {
    sampling_rate_ = new_sampling_rate;
}

void BasebandThread::run() {
    std::array<int8_t, host_m4::kMaxBlockSamples * 2> block{};
    while (!chThdShouldTerminate()) {
        size_t samples = 0;
        uint32_t rate = 0;
        const size_t bytes =
            host_m4::wait_block(block.data(), block.size(), &samples, &rate);
        if (bytes == 0) {
            chThdSleepMilliseconds(5);
            continue;
        }
        buffer_c8_t buffer{reinterpret_cast<complex8_t*>(block.data()), samples, rate};
        {
            static unsigned long dbg_exec = 0;
            dbg_exec++;
            if ((dbg_exec % 256) == 0)
                std::fprintf(stderr, "host_m4: execute blocks=%lu samples=%zu rate=%u\n",
                             dbg_exec, samples, rate);
        }
        if (baseband_processor_) baseband_processor_->execute(buffer);
        host_m4::after_execute();
    }
}

/* ------------------------------------------------------------------------- */
/* Host RSSIThread: the processors declare one, but the RSSI path needs the     */
/* device ADC. The thread is not started, so the RSSI widget stays at rest      */
/* while packet decoding (which does not depend on it) runs normally.           */
/* ------------------------------------------------------------------------- */
Thread* RSSIThread::thread = nullptr;

RSSIThread::RSSIThread(bool auto_start, tprio_t priority)
    : priority_{priority} {
    (void)auto_start;
}

RSSIThread::~RSSIThread() = default;

void RSSIThread::start() {}
void RSSIThread::run() {}

/* ------------------------------------------------------------------------- */
/* rf::rssi / rf::rssi::dma: device ADC + DMA. Inert on the host.               */
/* ------------------------------------------------------------------------- */
namespace rf {
namespace rssi {

void init() {}
void start() {}
void stop() {}

namespace dma {

void init() {}
void allocate(size_t, size_t) {}
void free() {}
void enable() {}
bool is_enabled() { return false; }
void disable() {}

buffer_t wait_for_buffer() {
    chThdSleepMilliseconds(100);
    return {};
}

}  // namespace dma
}  // namespace rssi
}  // namespace rf

/* ------------------------------------------------------------------------- */
/* audio::dma: device I2S0 codec DMA. The host forwards whatever the firmware's */
/* AudioOutput writes into its DMA buffer to the desktop PCM sink, and turns    */
/* the beep requests into host beeps.                                           */
/* ------------------------------------------------------------------------- */
namespace audio {
namespace dma {

namespace {
/* The firmware fills one 32-sample transfer per call and never tells us when
 * it is done; flushing the previous transfer on the next call adds a single
 * block of latency, which is inaudible. */
std::array<sample_t, 32> g_tx{};
std::array<sample_t, 32> g_rx{};
std::array<int16_t, 32> g_mono{};
bool g_has_pending = false;
constexpr uint32_t kHostAudioRate = 24000;
}  // namespace

void init_audio_in() {}
void init_audio_out() {}
void shrink_tx_buffer(bool) {}

void disable() {
    if (g_has_pending) {
        for (size_t i = 0; i < g_mono.size(); i++) g_mono[i] = g_tx[i].left;
        host_audio::write_pcm(g_mono.data(), g_mono.size(), kHostAudioRate);
        g_has_pending = false;
    }
    host_audio::stop_pcm();
}

void beep_start(uint32_t freq, uint32_t sample_rate, uint32_t beep_duration_ms) {
    host_audio::play_beep(freq, sample_rate, beep_duration_ms);
}

void beep_stop() {
    host_audio::stop();
}

buffer_t tx_empty_buffer() {
    if (g_has_pending) {
        for (size_t i = 0; i < g_mono.size(); i++) g_mono[i] = g_tx[i].left;
        host_audio::write_pcm(g_mono.data(), g_mono.size(), kHostAudioRate);
    }
    g_has_pending = true;
    return {g_tx.data(), g_tx.size()};
}

buffer_t rx_empty_buffer() {
    return {g_rx.data(), g_rx.size()};
}

}  // namespace dma
}  // namespace audio

/* ------------------------------------------------------------------------- */
/* lpc43xx::rtc::RTC::now()                                                     */
/*                                                                             */
/* In the M0 build `Timestamp` is `lpc43xx::rtc::RTC` (see common/buffer.hpp),  */
/* so packet_builder and proc_pocsag2 stamp decoded packets through             */
/* Timestamp::now() -> RTC::now(). The M4 build defines Timestamp::now()        */
/* itself in buffer.cpp; the M0 build needs this definition instead. The        */
/* declaration lives in host_forced.hpp; here the host wall clock is packed     */
/* into the same tv_date/tv_time layout the RTC getters read.                   */
/* ------------------------------------------------------------------------- */
namespace lpc43xx {
namespace rtc {

RTC RTC::now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return RTC{
        static_cast<uint32_t>(tm.tm_year + 1900),
        static_cast<uint32_t>(tm.tm_mon + 1),
        static_cast<uint32_t>(tm.tm_mday),
        static_cast<uint32_t>(tm.tm_hour),
        static_cast<uint32_t>(tm.tm_min),
        static_cast<uint32_t>(tm.tm_sec)};
}

}  // namespace rtc
}  // namespace lpc43xx

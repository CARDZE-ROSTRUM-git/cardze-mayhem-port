/*
 * Host audio output for the desktop PortaPack (see host_audio.hpp).
 *
 * Beeps requested through baseband::request_audio_beep() are rendered into the
 * host sound card by an SDL audio callback that synthesises a sine tone at the
 * device's own sample rate. Because the tone is generated directly at the device
 * rate there is no resampling and the pitch is exact regardless of whether the
 * card runs at 48 kHz or 44.1 kHz.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "host_audio.hpp"

/* hal.h (force-included) defines __I/__O/__IO as LPC43xx register qualifiers;
 * SDL's <immintrin.h> reuses those tokens. Drop them before SDL is seen, exactly
 * as platform/src/lcd_sdl.cpp does. */
#undef __I
#undef __O
#undef __IO

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace host_audio {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

SDL_AudioDeviceID g_device = 0;
bool g_enabled = false;
int g_rate = 48000;

/* Receive-audio ring. The host baseband producer thread appends demodulated
 * mono PCM at the firmware's audio rate; the SDL callback resamples it to the
 * device rate. Single-producer/single-consumer, so only the two index atomics
 * need ordering. Indices are free-running counters; the ring index is masked. */
constexpr size_t kPcmRing = 1u << 15; /* power of two */
int16_t g_pcm_ring[kPcmRing];
std::atomic<uint64_t> g_pcm_write{0}; /* producer */
std::atomic<uint64_t> g_pcm_read{0};  /* consumer */
std::atomic<uint32_t> g_pcm_rate{24000};
std::atomic<bool> g_pcm_enabled{false};

/* Callback-only resampling phase (in source samples, relative to g_pcm_read). */
double cb_pcm_pos = 0.0;
bool cb_pcm_had_data = false;

/* Written by the UI thread, read by the audio callback thread. The callback
 * owns the phase and the smoothed amplitude/frequency; the atomics only hand
 * over the targets, so no lock is needed. */
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_freq{1000};
std::atomic<uint32_t> g_generation{0};
std::atomic<float> g_amplitude{0.0f};
std::atomic<bool> g_has_deadline{false};
std::atomic<int64_t> g_deadline_ms{0};

/* Whole-output mute, driven by the navigation bar's speaker toggle through the
 * host codec's headphone/speaker lines. The callback emits silence while set. */
std::atomic<bool> g_muted{false};

/* Callback-thread state. */
uint32_t cb_generation = 0;
double cb_phase = 0.0;
float cb_amp = 0.0f;
float cb_freq = 1000.0f;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* Pulls demodulated PCM from the ring, resampled to the device rate. Returns
 * false when the ring is (momentarily) empty so the caller can fall back to the
 * beep synthesiser. */
bool render_pcm(int16_t* out, int count) {
    if (!g_pcm_enabled.load(std::memory_order_relaxed)) return false;

    const uint64_t write = g_pcm_write.load(std::memory_order_acquire);
    uint64_t read = g_pcm_read.load(std::memory_order_relaxed);
    const uint64_t avail = write - read;
    if (avail == 0) return false;

    const double src_rate = static_cast<double>(g_pcm_rate.load(std::memory_order_relaxed));
    const double step = (src_rate > 0.0) ? (src_rate / g_rate) : 1.0;

    /* Re-sync if the producer overwrote samples we had not read (underrun). */
    if (avail > kPcmRing) {
        cb_pcm_pos = 0.0;
        read = write;
        g_pcm_read.store(read, std::memory_order_release);
        return false;
    }

    for (int i = 0; i < count; ++i) {
        const uint64_t pos = static_cast<uint64_t>(cb_pcm_pos);
        if (pos + 1 >= avail) {
            /* Source exhausted mid-buffer: hold the last sample for the rest and
             * drop the consumed prefix so indices cannot grow without bound. */
            const int16_t last = g_pcm_ring[(read + (avail ? avail - 1 : 0)) & (kPcmRing - 1)];
            for (; i < count; ++i) out[i] = last;
            break;
        }
        const int16_t s0 = g_pcm_ring[(read + pos) & (kPcmRing - 1)];
        const int16_t s1 = g_pcm_ring[(read + pos + 1) & (kPcmRing - 1)];
        const double frac = cb_pcm_pos - static_cast<double>(pos);
        out[i] = static_cast<int16_t>(s0 + (s1 - s0) * frac);
        cb_pcm_pos += step;
    }

    const uint64_t consumed = static_cast<uint64_t>(cb_pcm_pos);
    cb_pcm_pos -= static_cast<double>(consumed);
    read += consumed;
    g_pcm_read.store(read, std::memory_order_release);
    return true;
}

void audio_callback(void* /*userdata*/, Uint8* stream, int len) {
    auto* out = reinterpret_cast<int16_t*>(stream);
    const int count = len / static_cast<int>(sizeof(int16_t));

    /* A navigation-bar mute silences everything: receive audio and beeps. Drain
     * the PCM ring so unmuting resumes live audio instead of replaying a stale
     * backlog, and reset the beep envelope. */
    if (g_muted.load(std::memory_order_relaxed)) {
        std::memset(out, 0, static_cast<size_t>(len));
        const uint64_t write = g_pcm_write.load(std::memory_order_acquire);
        g_pcm_read.store(write, std::memory_order_release);
        cb_pcm_pos = 0.0;
        cb_pcm_had_data = false;
        cb_amp = 0.0f;
        return;
    }

    /* Receive audio, when the baseband core is producing it, takes over the
     * output entirely - a beep and the demodulated signal are never mixed. */
    if (render_pcm(out, count)) {
        cb_pcm_had_data = true;
        return;
    }
    if (cb_pcm_had_data) {
        /* The PCM queue just ran dry (end of an audio mode or an underrun): emit
         * silence for this callback and reset the beep generator next time. */
        std::memset(out, 0, static_cast<size_t>(len));
        cb_pcm_had_data = false;
        cb_generation = g_generation.load(std::memory_order_relaxed);
        cb_amp = 0.0f;
        return;
    }

    const bool active = g_active.load(std::memory_order_relaxed);
    const uint32_t generation = g_generation.load(std::memory_order_relaxed);
    const float target_freq = static_cast<float>(g_freq.load(std::memory_order_relaxed));
    const float target_amp = active ? g_amplitude.load(std::memory_order_relaxed) : 0.0f;

    if (generation != cb_generation) {
        cb_generation = generation;
        cb_phase = 0.0; /* restart the tone so every beep begins at zero crossing */
    }

    /* A short slew keeps the amplitude and frequency from stepping and clicking,
     * mirroring the click-free envelope the codec path would give. */
    const float slew = 0.0016f;

    for (int i = 0; i < count; ++i) {
        cb_amp += (target_amp - cb_amp) * slew;
        cb_freq += (target_freq - cb_freq) * slew;

        double sample = std::sin(cb_phase) * cb_amp;
        cb_phase += kTwoPi * static_cast<double>(cb_freq) / g_rate;
        if (cb_phase >= kTwoPi) cb_phase -= kTwoPi;

        out[i] = static_cast<int16_t>(sample * 32767.0);
    }
}

bool ensure_device() {
    if (g_enabled) return true;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("host_audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_callback;

    SDL_AudioSpec have{};
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                       SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (g_device == 0) {
        SDL_Log("host_audio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }

    g_rate = have.freq > 0 ? have.freq : want.freq;
    SDL_PauseAudioDevice(g_device, 0); /* start the callback thread */
    g_enabled = true;
    return true;
}

float db_to_amplitude(int db) {
    if (db >= 0) return 1.0f;
    if (db <= -60) return 0.0f;
    return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}

}  // namespace

void play_beep(uint32_t freq, uint32_t /*sample_rate*/, uint32_t duration_ms) {
    if (!ensure_device()) return;
    if (freq == 0) freq = 1000;

    g_freq.store(freq, std::memory_order_relaxed);
    g_generation.fetch_add(1, std::memory_order_relaxed);
    if (duration_ms > 0) {
        g_deadline_ms.store(now_ms() + static_cast<int64_t>(duration_ms),
                            std::memory_order_relaxed);
        g_has_deadline.store(true, std::memory_order_relaxed);
    } else {
        g_has_deadline.store(false, std::memory_order_relaxed);
    }
    /* Set active last so the callback never sees a fresh deadline with a stale
     * "inactive" flag. */
    g_active.store(true, std::memory_order_relaxed);
}

void stop() {
    g_active.store(false, std::memory_order_relaxed);
    g_has_deadline.store(false, std::memory_order_relaxed);
    if (g_enabled) SDL_ClearQueuedAudio(g_device);
}

void write_pcm(const int16_t* samples, size_t count, uint32_t sample_rate) {
    if (!samples || count == 0) return;
    if (!ensure_device()) return;

    g_pcm_rate.store(sample_rate ? sample_rate : 24000, std::memory_order_relaxed);

    uint64_t write = g_pcm_write.load(std::memory_order_relaxed);
    const uint64_t read = g_pcm_read.load(std::memory_order_acquire);
    /* Never let the writer slip more than a ring ahead of the reader. */
    if (write - read + count > kPcmRing) {
        write = read + kPcmRing - count;
    }
    for (size_t i = 0; i < count; ++i) {
        g_pcm_ring[(write + i) & (kPcmRing - 1)] = samples[i];
    }
    g_pcm_write.store(write + count, std::memory_order_release);

    /* A playing beep would otherwise keep the ring drained by the fallback. */
    g_pcm_enabled.store(true, std::memory_order_relaxed);
}

void stop_pcm() {
    g_pcm_enabled.store(false, std::memory_order_relaxed);
    const uint64_t write = g_pcm_write.load(std::memory_order_relaxed);
    g_pcm_read.store(write, std::memory_order_release);
    cb_pcm_pos = 0.0;
}

void set_volume_db(int db) {
    g_amplitude.store(db_to_amplitude(db), std::memory_order_relaxed);
}

void set_muted(bool muted) {
    g_muted.store(muted, std::memory_order_relaxed);
}

void tick() {
    if (!g_has_deadline.load(std::memory_order_relaxed)) return;
    if (now_ms() >= g_deadline_ms.load(std::memory_order_relaxed)) {
        g_has_deadline.store(false, std::memory_order_relaxed);
        g_active.store(false, std::memory_order_relaxed);
    }
}

void shutdown() {
    if (g_device != 0) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    g_enabled = false;
}

}  // namespace host_audio

/*
 * Host replacement for firmware/application/irq_controls.cpp.
 *
 * On the device a 1 kHz GPT interrupt scans the CPLD for the six navigation
 * switches, the rotary encoder and the resistive touch panel. On the desktop
 * the same debounce/encoder state machines are driven by an SDL event pump:
 *
 *   - switches: keyboard arrows / WASD / Enter / Escape, plus the on-screen
 *     PortaPack H2 buttons.
 *   - encoder: keyboard Q/E, the mouse wheel, and the on-screen wheel.
 *   - touch:   the mouse, translated into a resistive-touch frame.
 *
 * The debounce (application/hw/debounce.*) and encoder
 * (application/hw/encoder.*) logic is reused unchanged.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#include "irq_controls.hpp"

#include "ch.h"
#include "debounce.hpp"
#include "encoder.hpp"
#include "event_m0.hpp"
#include "hal.h"
#include "portapack_persistent_memory.hpp"
#include "portapack_shared_memory.hpp"
#include "touch.hpp"
#include "utility.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <mutex>

#include "host_input.hpp"
#include "host_audio.hpp"
#include "host_m4.hpp"
#include "host_baseband.hpp"
#include "host_lcd.hpp"
#include "host_battery.hpp"

using namespace portapack;

namespace {

Thread* thread_controls_event = nullptr;

/* Dedicated consumer of the baseband command channel; see
 * host_baseband_service_start(). Declared here so both the input tick and the
 * service thread operate on the same shared_memory slot. */
Thread* thread_baseband_service = nullptr;
std::atomic<bool> baseband_service_running{false};

std::array<Debounce, 6> switch_debounce;
EncoderDebounce encoder_debounce;
Encoder encoder;

std::atomic<uint32_t> encoder_position{0};
touch::Frame touch_frame;

/* Raw switch bitmap as the CPLD would report it: bit0=Right, bit1=Left,
 * bit2=Down, bit3=Up, bit4=Sel, bit5=Dfu, bits 6..7 = encoder phase A/B. */
uint8_t switches_raw{0};
uint8_t injected_switch{0};
uint8_t injected_encoder{0};

bool touch_down{false};

}  // namespace

namespace host_input {
InputState state;
}  // namespace host_input

namespace {

void refresh_switches_from_host() {
    using host_input::Btn;
    const auto& s = host_input::state;

    uint8_t raw = 0;
    if (s.button(Btn::Right)) raw |= 1U << 0;
    if (s.button(Btn::Left)) raw |= 1U << 1;
    if (s.button(Btn::Down)) raw |= 1U << 2;
    if (s.button(Btn::Up)) raw |= 1U << 3;
    if (s.button(Btn::Select)) raw |= 1U << 4;
    if (s.button(Btn::Dfu)) raw |= 1U << 5;
    if (s.enc_a) raw |= 1U << 6;
    if (s.enc_b) raw |= 1U << 7;
    switches_raw = raw;
}

/*
 * Builds a synthetic resistive-touch frame that the upstream touch::Manager
 * accepts as a real press at the requested panel coordinates.
 *
 * touch::calibration maps raw digitizer coordinates to panel pixels through an
 * affine matrix. The default mapping (and any stored re-calibration) is only
 * defined over the digitizer's own range, so feeding it the panel pixels
 * directly lands the tap in the wrong place. Instead the calibration is
 * inverted: three probes inside the unclipped region recover the linear part,
 * and the inverse yields the raw sample that maps back to the requested pixel.
 */
touch::DigitizerPoint pixel_to_digitizer(int px, int py) {
    const auto& cal = portapack::persistent_memory::touch_calibration();

    /* Probe points are well inside the calibration triangle for every shipped
     * panel, so translate() does not clip them and the affine fit is exact. */
    const touch::DigitizerPoint da{500, 400};
    const touch::DigitizerPoint db{600, 400};
    const touch::DigitizerPoint dc{500, 500};
    const auto pa = cal.translate(da);
    const auto pb = cal.translate(db);
    const auto pc = cal.translate(dc);

    const float colx_x = static_cast<float>(pb.x() - pa.x());
    const float colx_y = static_cast<float>(pb.y() - pa.y());
    const float coly_x = static_cast<float>(pc.x() - pa.x());
    const float coly_y = static_cast<float>(pc.y() - pa.y());

    const float det = colx_x * coly_y - colx_y * coly_x;
    if (det == 0.0f) return da; /* degenerate calibration: fall back to a probe */

    const float vx = static_cast<float>(px - pa.x());
    const float vy = static_cast<float>(py - pa.y());

    const float u = (vx * coly_y - vy * coly_x) / det;
    const float w = (colx_x * vy - colx_y * vx) / det;

    return {
        static_cast<int32_t>(500.0f + 100.0f * u),
        static_cast<int32_t>(400.0f + 100.0f * w)};
}

void refresh_touch_from_host() {
    const auto& s = host_input::state;
    if (s.touch) {
        const auto d = pixel_to_digitizer(s.touch_x, s.touch_y);

        /* The filters average metrics.x/y scaled by 1024, and the calibration
         * consumes those sums directly. With x_min/y_min = 0 and x_max/y_max =
         * 1024 the normalized value the frame produces is exactly d/1024, so the
         * digitizer point the calibration sees is the one computed above. */
        const uint16_t dx = static_cast<uint16_t>(std::clamp<int32_t>(d.x, 0, 1024));
        const uint16_t dy = static_cast<uint16_t>(std::clamp<int32_t>(d.y, 0, 1024));

        touch_frame.pressure.xp = 900;
        touch_frame.pressure.xn = 100;
        touch_frame.pressure.yp = 900;
        touch_frame.pressure.yn = 100;

        touch_frame.x.xp = 1024;
        touch_frame.x.xn = 0;
        touch_frame.x.yp = dx;
        touch_frame.x.yn = dx;

        touch_frame.y.yp = 0;
        touch_frame.y.yn = 1024;
        touch_frame.y.xp = dy;
        touch_frame.y.xn = dy;

        touch_frame.touch = true;
        touch_down = true;
    } else {
        touch_frame = touch::Frame{};
        touch_down = false;
    }
}

bool encoder_read() {
    auto delta = encoder.update(encoder_debounce.state()) * encoder_debounce.rotation_rate();

    if (injected_encoder > 0) {
        if (injected_encoder == 1) delta = -1;
        if (injected_encoder == 2) delta = 1;
        injected_encoder = 0;
    }

    if (delta != 0) {
        encoder_position += delta;
        return true;
    }
    return false;
}

/* Emulates the M4 baseband core consuming the command channel.
 *
 * The UI thread sends commands through baseband_api::send_message(), which
 * stores the message in shared_memory.baseband_message and then spins until the
 * baseband core clears the slot (upstream: the M4 event loop reads it in
 * handle_baseband_queue()). There is no M4 on the host, so this clears the slot
 * on the baseband's behalf; without it send_message() spins ~4 billion times and
 * then panics with "Baseband Send Fail".
 *
 * Crucially this runs on its own thread, not from the SDL loop: the SDL loop
 * stops whenever the window closes or the process is quitting, and the
 * receiver app's destructor calls baseband::shutdown() on the UI thread during
 * teardown -- if draining depended on the SDL loop, that send_message() would
 * never be drained and the process would abort with "Baseband Send Fail".
 */
bool service_baseband_channel() {
    const Message* const message = shared_memory.baseband_message;
    if (message == nullptr) return false;

    host_baseband::handle_message(message);

    /* Forward the command to the real M4 processor image (if one is running):
     * this is the host stand-in for the M4 event loop's on_message(). Decoders
     * learn their modulation/rate from SubGhzFPRxConfigure and similar here. */
    if (message->id == Message::ID::Shutdown) {
        host_m4::stop_image();
    } else {
        host_m4::on_message(message);
    }

    switch (message->id) {
        case Message::ID::AudioBeep: {
            const auto& beep = *reinterpret_cast<const AudioBeepMessage*>(message);
            host_audio::play_beep(beep.freq, beep.sample_rate, beep.duration_ms);
            break;
        }

        case Message::ID::RequestSignal: {
            const auto& req = *reinterpret_cast<const RequestSignalMessage*>(message);
            if (req.signal == RequestSignalMessage::Signal::BeepStopRequest) {
                host_audio::stop();
            }
            break;
        }

        default:
            break;
    }

    shared_memory.baseband_message = nullptr;
    return true;
}

}  // namespace

/* Thread body: poll the command channel at a fine cadence so send_message()'s
 * spin finishes promptly, exactly as the real M4 event loop would. */
msg_t baseband_service_thread(void*) {
    while (baseband_service_running.load()) {
        if (!service_baseband_channel()) {
            chThdSleepMicroseconds(200);
        }
    }
    return 0;
}

void host_baseband_service_start() {
    if (thread_baseband_service) return;
    baseband_service_running = true;
    thread_baseband_service = chThdCreateFromHeap(
        nullptr, 4096, NORMALPRIO + 10, baseband_service_thread, nullptr);
    /* Bring up the host baseband core (the M4 stand-in): it runs the receive
     * DSP and feeds the waterfall/capture from the SDR backend. */
    host_baseband::start();
}

void host_baseband_service_stop() {
    if (!thread_baseband_service) return;
    baseband_service_running = false;
    chThdTerminate(thread_baseband_service);
    chThdWait(thread_baseband_service);
    thread_baseband_service = nullptr;
    /* Drain anything still queued so a late sender never blocks. */
    service_baseband_channel();
}

/* Called by the SDL thread (main loop) at roughly 1 kHz. This replaces
 * timer0_callback(): it snapshots the host input state, feeds the debouncers
 * and signals the event loop. */
void host_controls_tick() {
    eventmask_t event_mask = 0;
    /* Follow the navigation-bar speaker toggle: the top-bar button (and the
     * Settings entry) record their state in persistent memory, and this is the
     * desktop stand-in for the codec's speaker/headphone enable lines. Reading
     * the flag directly - rather than the codec lines - keeps the mute under the
     * user's control alone: the DMA stop/start a view does on entry must not
     * silence the host. The host has a single output, so "speaker disabled" and
     * the master mute are collapsed into the same silence. */
    host_audio::set_muted(portapack::persistent_memory::config_audio_mute() ||
                          portapack::persistent_memory::config_speaker_disable());

    /* Keep a continuous (duration_ms == 0) beep fed; games use timed beeps, so
     * this is normally a cheap no-op. */
    host_audio::tick();

    /* Push the host's battery state to the status bar (~1 Hz). */
    host_battery::tick();

    /* Emit at most one encoder phase step this tick. The upstream debouncer only
     * accepts a new level after it has been stable for a few 1 kHz samples, so
     * requested detents are spread across ticks rather than applied at once. */
    host_input::state.step_encoder();
    refresh_switches_from_host();

    /* Note whether the touch is being pressed or released *before* refreshing, so
     * the event loop is signalled on both edges. The upstream touch::Manager only
     * leaves its TouchDetected state when it is fed a frame with touch == false;
     * if the mask were raised only while touch was down, a release would never
     * reach feed() and the pressed widget would stick. */
    const bool prev_touch_down = touch_down;
    refresh_touch_from_host();
    if (touch_down != prev_touch_down) event_mask |= EVT_MASK_TOUCH;
    if (touch_down) event_mask |= EVT_MASK_TOUCH;

    bool switch_changed = false;
    for (size_t i = 0; i < switch_debounce.size(); ++i) {
        const uint8_t bit = (switches_raw >> i) & 0x01;
        switch_changed |= switch_debounce[i].feed(bit);
    }
    if (switch_changed || injected_switch > 0) event_mask |= EVT_MASK_SWITCHES;

    if (encoder_debounce.feed(static_cast<uint8_t>(switches_raw >> 6)) || encoder_read()) {
        event_mask |= EVT_MASK_ENCODER;
    }

    if (event_mask && thread_controls_event) {
        chEvtSignal(thread_controls_event, event_mask);
    }
}

/* Wakes the display if it is asleep. The firmware's display-sleep path gates the
 * encoder and touch handlers out entirely (see EventDispatcher::dispatch), so on
 * the device only a key press wakes the screen. On the desktop any input should
 * do it, so the SDL layer calls this for wheel/button/touch events. It only acts
 * when the backlight is actually off, so it can be called unconditionally.
 * Returns true if it woke the display, so the caller can swallow the wake event
 * and avoid acting on a control the user did not mean to press. */
bool host_wake_display() {
    if (!host_lcd::backlight_on()) {
        EventDispatcher::set_display_sleep(false);
        return true;
    }
    return false;
}

uint8_t swizzled_switches() {
    return switches_raw;
}
void controls_init() {
    thread_controls_event = chThdSelf();

#ifdef PRALINE
    for (auto i = Switch::Right; i <= Switch::Up; incr(i))
#else
    for (auto i = Switch::Right; i <= Switch::Sel; incr(i))
#endif
        switch_debounce[toUType(i)].enable_repeat();
}

SwitchesState get_switches_pressed() {
    SwitchesState result;
    for (size_t i = 0; i < result.size(); i++)
        result[i] = switch_debounce[i].pressed();
    return result;
}

SwitchesState get_switches_state() {
    SwitchesState result;

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = switch_debounce[i].state();

    if (injected_switch > 0 && injected_switch <= 6) {
        result[injected_switch - 1] = 1;
        injected_switch = 0xff;
    } else if (injected_switch == 0xff) {
        injected_switch = 0x00;
    }

    return result;
}

SwitchesState get_switches_repeat_config() {
    SwitchesState result;
    for (size_t i = 0; i < result.size(); i++)
        result[i] = switch_debounce[i].get_repeat_enabled();
    return result;
}

void set_switches_repeat_config(SwitchesState switch_config) {
    for (size_t i = 0; i < switch_config.size(); i++)
        switch_debounce[i].set_enable_repeat(switch_config[i]);
}

SwitchesState get_switches_long_press_config() {
    SwitchesState result;
    for (size_t i = 0; i < result.size(); i++)
        result[i] = switch_debounce[i].get_long_press_enabled();
    return result;
}

void set_switches_long_press_config(SwitchesState switch_config) {
    for (size_t i = 0; i < switch_config.size(); i++)
        switch_debounce[i].set_long_press_enabled(switch_config[i]);
}

bool switch_is_long_pressed(Switch s) {
    return switch_debounce[toUType(s)].long_press_occurred();
}

EncoderPosition get_encoder_position() {
    return encoder_position;
}

touch::Frame get_touch_frame() {
    return touch_frame;
}

namespace control {
namespace debug {

uint8_t switches() {
    return switches_raw;
}

void inject_switch(uint8_t button) {
    if (button <= 6)
        injected_switch = button;
    else
        injected_encoder = button - 6;
    EventDispatcher::events_flag(EVT_MASK_SWITCHES | EVT_MASK_ENCODER);
}

}  // namespace debug
}  // namespace control

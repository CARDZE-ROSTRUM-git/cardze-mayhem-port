/*
 * Host input state shared between the SDL main loop and input_controls.cpp.
 *
 * The SDL layer fills this in from keyboard and mouse events; the controls
 * layer samples it once per simulated 1 kHz scan, exactly where the device
 * would read the CPLD through portapack::IO.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_INPUT_HPP__
#define __HOST_INPUT_HPP__

#include <cstdint>

namespace host_input {

enum class Btn : uint8_t {
    Right = 0,
    Left = 1,
    Down = 2,
    Up = 3,
    Select = 4,
    Dfu = 5,
};

struct InputState {
    /* Navigation buttons, indexed by Btn. */
    bool buttons[6]{false, false, false, false, false, false};

    /* Rotary encoder. The upstream Encoder/debounce logic walks a quadrature
     * sequence in Gray order (00 -> 01 -> 11 -> 10 clockwise), NOT a binary
     * count, and only registers a change once an input level has been stable for
     * a few 1 kHz samples. The host therefore *queues* requested detents and
     * emits at most one per step_encoder() call, spaced a few ticks apart, so
     * the debouncer sees the same held levels a real dial produces. */
    int step_queue{0};      /* signed pending detents (positive = clockwise) */
    int step_spacing{0};    /* ticks left before the next detent may be emitted */
    int8_t encoder_phase{0};  /* index into the Gray cycle (0..3) */

    /* Current quadrature levels (A = bit0, B = bit1), refreshed by step_encoder(). */
    bool enc_a{false};
    bool enc_b{false};

    /* Mouse-as-touchscreen. Coordinates are panel pixels (portrait). */
    bool touch{false};
    int touch_x{0};
    int touch_y{0};

    bool button(Btn b) const { return buttons[static_cast<int>(b)]; }
    void set(Btn b, bool v) { buttons[static_cast<int>(b)] = v; }

    /* Requests `steps` encoder detents (positive = clockwise). Each detent is
     * two phase transitions for the upstream Encoder (a "start" and an "end"),
     * so two steps are queued per detent; they are emitted by step_encoder() on
     * subsequent ticks. */
    void rotate(int steps) { step_queue += steps * 2; }

    /* Advances the encoder by at most one detent per call (each call = one
     * 1 kHz tick). Returns true when a level changed, so the caller can signal
     * the event loop. Walks the Gray cycle 00,01,11,10 so the upstream
     * transition map always sees a consistent direction. */
    bool step_encoder() {
        static constexpr int8_t kGray[4] = {0, 1, 3, 2};  /* bit0 = A, bit1 = B */

        if (step_spacing > 0) {
            --step_spacing;
            return false;
        }

        int dir = 0;
        if (step_queue > 0) {
            dir = +1;
            --step_queue;
        } else if (step_queue < 0) {
            dir = -1;
            ++step_queue;
        } else {
            return false;
        }

        encoder_phase = static_cast<int8_t>((encoder_phase + dir) & 3);
        const int8_t bits = kGray[encoder_phase & 3];
        enc_a = (bits & 1) != 0;
        enc_b = (bits & 2) != 0;
        /* Leave the new level stable for a few samples before the next detent,
         * matching the spacing of a real dial and satisfying the debouncer. */
        step_spacing = 3;
        return true;
    }

    void press(Btn b) { set(b, true); }
    void release(Btn b) { set(b, false); }
    void release_all() {
        for (auto& v : buttons) v = false;
    }
};

extern InputState state;

}  // namespace host_input

/* Called by the SDL main loop at approximately 1 kHz: snapshots the host input
 * state, feeds the debouncers and signals the event loop. */
void host_controls_tick();

/* Host display-sleep helper. On the device the display sleeps when the user
 * presses the Sleep button (or the backlight timer expires); any key press wakes
 * it, but the encoder and touch are gated out while asleep, so a wheel turn or a
 * tap would NOT wake it. The desktop wants any input to wake the screen, so the
 * SDL layer calls this on every input event; it wakes the display only when it
 * is currently asleep. Runs on the SDL (main) thread.
 *
 * Returns true when the display was asleep and this call just woke it. In that
 * case the waking event must be swallowed by the caller: on the device the
 * input that wakes the screen does not also act on the UI, so a waking tap must
 * not press the button or touch target hidden behind it. */
bool host_wake_display();

/* Starts/stops the dedicated thread that emulates the M4 baseband core's
 * command consumer (upstream event_m4.cpp). It must keep running for the whole
 * life of the process, independently of the SDL loop, because baseband_api's
 * send_message() blocks until the command channel is drained -- including while
 * the SDL loop is not spinning (during UI teardown, for instance, where the
 * receiver app's destructor calls baseband::shutdown()). Started right after
 * portapack::init(); stopped just before the process exits. */
void host_baseband_service_start();
void host_baseband_service_stop();

#endif /* __HOST_INPUT_HPP__ */

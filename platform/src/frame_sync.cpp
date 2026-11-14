/*
 * Host replacement for firmware/application/irq_lcd_frame.cpp.
 *
 * On the device the LCD tearing-effect line fires a GPIO interrupt once per
 * panel refresh, which is the heartbeat of the whole UI (repaint, animation,
 * backlight timer). Here a 60 Hz thread plays that role.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#include "event_m0.hpp"
#include "ch.h"

#include <atomic>
#include <thread>

namespace {

Thread* thread_lcd_frame_event = nullptr;
std::atomic<bool> frame_sync_running{false};

msg_t frame_sync_thread(void* arg) {
    (void)arg;
    while (frame_sync_running.load()) {
        if (thread_lcd_frame_event) {
            chEvtSignal(thread_lcd_frame_event, EVT_MASK_LCD_FRAME_SYNC);
        }
        chThdSleepMilliseconds(1000 / 60);
    }
    return 0;
}

Thread* frame_thread = nullptr;

}  // namespace

/* Replaces lcd_frame_sync_configure(): remember the event-loop thread and start
 * the 60 Hz frame clock. */
void lcd_frame_sync_configure() {
    thread_lcd_frame_event = chThdSelf();

    if (!frame_sync_running.exchange(true)) {
        frame_thread = chThdCreateFromHeap(nullptr, 1024, NORMALPRIO, frame_sync_thread, nullptr);
    }
}

void lcd_frame_sync_stop() {
    frame_sync_running = false;
    if (frame_thread) {
        chThdTerminate(frame_thread);
        chThdWait(frame_thread);
        frame_thread = nullptr;
    }
}

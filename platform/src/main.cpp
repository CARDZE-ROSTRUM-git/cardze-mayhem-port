/*
 * Host entry point: starts the firmware's own UI event loop on a background
 * thread, then drives the SDL window from the main thread.
 *
 * This replaces firmware/application/main.cpp, which boots the MCU (clock tree,
 * CPLD, USB) before starting the same event loop. On the desktop the boot work
 * is done by platform/src/portapack.cpp (portapack::init) and hardware_stubs;
 * everything from the event loop onward is the unmodified upstream UI:
 *
 *   event_loop() mirrors upstream main():
 *     - creates the ui::Context and ui::SystemView (the whole menu tree),
 *     - publishes them through system_view_ptr (read by several apps),
 *     - builds the EventDispatcher, wires it to the USB serial and I2C managers,
 *     - runs the navigation view's autostart (which loads the main menu),
 *     - then blocks in EventDispatcher::run().
 *
 * The SDL main thread pumps input at ~1 kHz (host_controls_tick) and repaints at
 * the display refresh rate, so the UI sees exactly the same event masks the
 * device would generate from its timer, CPLD scan and LCD frame interrupt.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "ch.h"
#include "hal.h"

#include "lcd_sdl.hpp"

#include "portapack.hpp"
#include "portapack_persistent_memory.hpp"

#include "core_control.hpp"
#include "event_m0.hpp"
#include "i2cdevmanager.hpp"
#include "irq_controls.hpp"
#include "irq_lcd_frame.hpp"
#include "lcd_ili9341.hpp"
#include "rtc_time.hpp"
#include "sd_card.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include "host_input.hpp"
#include "host_lcd.hpp"
#include "host_launcher.hpp"
#include "host_baseband.hpp"
#include "host_settings.hpp"
#include "sdr_device.hpp"

/* The Replay app's view, pulled in only for the MAYHEM_AUTOPLAY_PPL diagnostic
 * hook below (it opens the app straight on a playlist so the transmit handshake
 * can be exercised without the touch UI). */
#include "ui_playlist.hpp"

/* The Remote app's view, pulled in only for the MAYHEM_OPEN_REMOTE diagnostic
 * hook below (it opens the app so its dead waterfall band can be reproduced
 * without driving the touch UI). */
#include "external/remote/ui_remote.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>

/* Published by the upstream event loop so that several apps can reach the
 * navigation tree; upstream defines it in application/main.cpp, which the host
 * build does not compile. */
ui::SystemView* system_view_ptr = nullptr;

/* Provided by platform/src/frame_sync.cpp. The upstream irq_lcd_frame.hpp only
 * declares the configure side; the host adds a stop for orderly shutdown. */
void lcd_frame_sync_stop();

namespace {

/* The upstream main() seeds a small LCG and exports rand()/srand() so apps that
 * expect the C library's are deterministic and lock-free. Keep the same. */
uint32_t random_seed_state = 123456789;

Thread* thread_ui = nullptr;

/* Replaces irq_rtc.cpp on the host: the device gets EVT_MASK_RTC_TICK from the
 * LPC43xx RTC second interrupt. irq_rtc.cpp is not built (it pokes the real RTC
 * and event router), so a 1 Hz thread delivers the same event to the UI thread,
 * which keeps the clock, the second tick and the backlight timeout alive. */
std::atomic<bool> tick_running{false};
Thread* thread_rtc_tick = nullptr;

msg_t rtc_tick_thread(void*) {
    while (tick_running.load()) {
        EventDispatcher::events_flag(EVT_MASK_RTC_TICK);
        chThdSleepMilliseconds(1000);
    }
    return 0;
}

/* Builds the whole UI and runs the event loop, exactly as upstream main(). */
msg_t ui_thread(void*) {
    static ui::Context context;
    static ui::SystemView system_view{
        context,
        portapack::display.screen_rect()};

    system_view_ptr = &system_view;

    /* The dispatcher records chThdSelf() as the event loop thread, so it must
     * be created on this thread. */
    EventDispatcher event_dispatcher{&system_view, context};

    /* Both of these latch chThdSelf() as their event target, so they belong on
     * the UI thread too; calling them from the SDL thread would post switch and
     * frame events to a thread that never waits for them. */
    controls_init();
    lcd_frame_sync_configure();

    static MessageHandlerRegistration message_handler_display_sleep{
        Message::ID::DisplaySleep,
        [&event_dispatcher](const Message* const) {
            event_dispatcher.set_display_sleep(true);
        }};

    portapack::setEventDispatcherToUSBSerial(&event_dispatcher);
    i2cdev::I2CDevManager::setEventDispatcher(&event_dispatcher);

    system_view.get_navigation_view()->handle_autostart();

    /* Diagnostic hook (MAYHEM_AUTOPLAY_PPL=<playlist path>): open the Replay
     * app straight on a playlist so the replay transmit handshake
     * (ReplayConfig -> FillRequest -> prefill -> FIFOData) can be exercised
     * without driving the touch UI. Env-gated, never enabled by default. */
    if (const char* ppl = std::getenv("MAYHEM_AUTOPLAY_PPL")) {
        if (ppl[0] != '\0') {
            /* The path is a FatFs-style path the port maps under MAYHEM_SD
             * (e.g. "/PLAYLIST/PLAY_0000.PPL"), not a host path. The shim's
             * std::filesystem path uses a UTF-16 native string and has no
             * const char* constructor; the paths here are plain ASCII. */
            std::u16string wide;
            for (const char* p = ppl; *p; ++p)
                wide.push_back(static_cast<char16_t>(*p));
            system_view.get_navigation_view()->push<ui::PlaylistView>(
                std::filesystem::path{wide});
        }
    }

    /* Diagnostic hook (MAYHEM_OPEN_REMOTE=1): open the Remote app straight away
     * so its waterfall band can be exercised without driving the touch UI.
     * Env-gated, never enabled by default. */
    if (const char* r = std::getenv("MAYHEM_OPEN_REMOTE")) {
        if (r[0] != '\0') {
            system_view.get_navigation_view()->push<ui::external_app::remote::RemoteAppView>();
        }
    }

    event_dispatcher.run();
    return 0;
}

}  // namespace

extern "C" int rand(void) {
    random_seed_state ^= random_seed_state << 13;
    random_seed_state ^= random_seed_state >> 17;
    random_seed_state ^= random_seed_state << 5;
    return static_cast<int>(random_seed_state & 0x7FFFFFFF);
}

extern "C" void srand(unsigned int seed) {
    random_seed_state = (seed == 0) ? 123456789 : seed;
}

int main(int argc, char** argv) {
    int scale = 2;
    if (argc > 1) {
        const int v = std::atoi(argv[1]);
        if (v >= 1 && v <= 3) scale = v;
    }

    if (!lcd_sdl::init("PortaPack Mayhem (desktop)", scale)) {
        std::fprintf(stderr, "mayhem-desktop: cannot open the SDL window\n");
        return 1;
    }

    /* Pre-boot setup window: pick the SD folder and the SDR device, then Start
     * or Quit. Runs before portapack::init() so the SD folder it returns can
     * still be installed (MAYHEM_SD + the persistent-memory mapping) before any
     * firmware code reads them. MAYHEM_SKIP_LAUNCHER=1 boots straight through,
     * which the installed launcher uses when it has already been configured. */
    std::string sd_dir;
    {
        if (const char* env = std::getenv("MAYHEM_SD")) {
            if (*env) sd_dir = env;
        } else if (const char* home = std::getenv("HOME")) {
            sd_dir = std::string(home) + "/mayhem-sd";
        } else {
            sd_dir = "mayhem-sd";
        }
    }

    if (std::getenv("MAYHEM_SKIP_LAUNCHER") == nullptr) {
        host_launcher::Result choice = host_launcher::run(sd_dir, scale);
        if (!choice.start) {
            lcd_sdl::shutdown();
            return 0;
        }
        sd_dir = choice.sd_dir;
        sdr::set_preferred_device(choice.driver, choice.device_args);
        /* Apply the chosen input mode and rotation: with the deck hidden the
         * window shrinks to just the screen before the firmware boots. */
        lcd_sdl::set_deck_visible(!choice.touch_only);
        lcd_sdl::set_rotation(choice.rotation);
    } else {
        /* No setup window this run: honour the settings saved by the last one
         * (rotation, input mode and key bindings). host_launcher::run() loads
         * the file when the setup window is shown; do it here for the direct
         * boot, and apply the deck/rotation choices the launcher would have. */
        host_settings::load();
        lcd_sdl::set_deck_visible(!host_settings::touch_only());
        lcd_sdl::set_rotation(host_settings::rotation());
    }

    /* Install the chosen folder: the FatFs shim and the persistent-memory
     * mapping both read MAYHEM_SD, so set it and re-point the (already mapped)
     * backup-RAM region at the chosen folder before portapack::init(). */
    ::setenv("MAYHEM_SD", sd_dir.c_str(), 1);
    host_regs::remap_backup_ram(sd_dir.c_str());

    /* Bring up the emulated device: LCD surface, backlight, RTC, SD card, audio
     * sink and the radio seam. This is the host portapack::init(). */
    if (portapack::init() != portapack::init_status_t::INIT_SUCCESS) {
        std::fprintf(stderr, "mayhem-desktop: portapack::init failed\n");
        lcd_sdl::shutdown();
        return 1;
    }

    Theme::SetTheme(static_cast<Theme::ThemeId>(portapack::persistent_memory::ui_theme_id()));

    /* Battery capacity typed in the setup window (0 = auto, read from the
     * device). Push it into the firmware's setting so the battery screen and
     * the charge estimate use it. */
    {
        const int batt_mah = host_settings::battery_mah();
        if (batt_mah > 0)
            portapack::persistent_memory::set_battery_cap_mah(static_cast<uint16_t>(batt_mah));
    }

    /* Second heartbeat (see rtc_tick_thread) and the UI thread itself. The frame
     * sync and controls init happen inside ui_thread, where chThdSelf() is
     * valid. */
    tick_running = true;
    thread_rtc_tick = chThdCreateFromHeap(nullptr, 4096, NORMALPRIO, rtc_tick_thread, nullptr);

    /* Dedicated consumer of the baseband command channel. It must run for the
     * whole life of the process, independent of the SDL loop: baseband_api's
     * send_message() blocks until the channel is drained, and the UI thread may
     * send during teardown (e.g. a receiver app's destructor calling
     * baseband::shutdown()) after the SDL loop has already stopped. The static
     * SystemView (which owns the open app) is destroyed at process exit, after
     * main() returns, so the thread is torn down from an atexit handler set up
     * here -- it therefore outlives every static destructor that could send. */
    host_baseband_service_start();

    thread_ui = chThdCreateFromHeap(nullptr, 64 * 1024, NORMALPRIO, ui_thread, nullptr);

    const auto tick_period = std::chrono::milliseconds(1);
    auto next_tick = std::chrono::steady_clock::now();

    /* The upstream splash screen (SystemView's SplashScreenView) waits for a
     * touch before it pops to the main menu, exactly as on the device. On the
     * desktop that reads as "the menu never opens", so the host advances past it
     * automatically after a short dwell. A tap or any key still dismisses it
     * sooner; the firmware's own UI code is untouched. Only done when the splash
     * is enabled, so a user with it turned off is never sent a stray keypress. */
    const bool splash_enabled = portapack::persistent_memory::config_splash();
    long loop_frame = 0;

    /* MAYHEM_AUTOPLAY_PPL also drives a Select keypress on the Replay view (see
     * ui_thread), so the splash auto-advance must not steal that frame. */
    const bool autoplay = std::getenv("MAYHEM_AUTOPLAY_PPL") != nullptr;

    /* Diagnostic hook (MAYHEM_SELFTEST=1): push synthetic input from inside this
     * process only, to exercise the whole path without touching the operator's
     * session. Never enabled by default. */
    const bool selftest = std::getenv("MAYHEM_SELFTEST") != nullptr;

    while (lcd_sdl::pump()) {
        /* ~1 kHz scan: feed the debouncers and signal the event loop. */
        host_controls_tick();

        /* Auto-advance the splash after ~3 s (see above). */
        if (splash_enabled && !autoplay && loop_frame == 3000) {
            lcd_sdl::inject_key_event(13 /* Enter: activates the Done button */, true);
        } else if (splash_enabled && !autoplay && loop_frame == 3100) {
            lcd_sdl::inject_key_event(13, false);
        }

        /* Autoplay: once the Replay view is up (its playlist load has run by
         * now), press Select to start transmitting. Driven through the normal
         * host_input path (not injected_switch) so it goes through the same
         * debounce + press/release edges a real button does; a single-shot
         * injected_switch pulse is too short for the debouncer to register. */
        if (autoplay && loop_frame == 1200) {
            host_input::state.set(host_input::Btn::Select, true);
        }
        if (autoplay && loop_frame == 1400) {
            host_input::state.set(host_input::Btn::Select, false);
        }

        /* Diagnostic injection only when explicitly requested. */
        if (selftest && loop_frame == 5000) {
            lcd_sdl::inject_mouse_click((16 + 120) * scale, (16 + 160) * scale);
        }

        ++loop_frame;

        /* Diagnostic hook: return from main after N frames so the process's
         * clean-exit path (static SystemView destructor -> app dtor ->
         * baseband::shutdown) can be exercised. Env-gated, never on by
         * default (see MAYHEM_SELFTEST above for the same pattern). */
        if (const char* ae = std::getenv("MAYHEM_AUTOEXIT_MS")) {
            const long ms = std::atol(ae);
            if (ms > 0 && loop_frame >= ms) break;
        }

        if (host_lcd::consume_dirty() || lcd_sdl::consume_redraw()) {
            lcd_sdl::render();
        }

        next_tick += tick_period;
        std::this_thread::sleep_until(next_tick);

        /* If a frame or input stalled us, do not spin trying to catch up. */
        const auto now = std::chrono::steady_clock::now();
        if (next_tick < now - std::chrono::milliseconds(50)) {
            next_tick = now;
        }
    }

    /* Window closed: ask the UI thread to leave its loop and wait for it. */
    EventDispatcher::request_stop();
    EventDispatcher::events_flag(ALL_EVENTS);
    if (thread_ui) {
        chThdWait(thread_ui);
        thread_ui = nullptr;
    }

    lcd_frame_sync_stop();

    tick_running = false;
    if (thread_rtc_tick) {
        chThdTerminate(thread_rtc_tick);
        chThdWait(thread_rtc_tick);
        thread_rtc_tick = nullptr;
    }

    /* NOTE: the baseband service thread is deliberately NOT stopped here. The
     * static SystemView (which owns any open app) is destroyed by exit() after
     * main() returns, and an open receiver app's destructor calls
     * baseband::shutdown() at that point. Stopping the consumer before then
     * would make that last send_message() spin out into "Baseband Send Fail".
     * The thread therefore keeps draining until the process is torn down. */

    portapack::shutdown();
    lcd_sdl::shutdown();

    return 0;
}

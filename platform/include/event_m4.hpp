/*
 * Host shadow of firmware/baseband/event_m4.hpp.
 *
 * The firmware's baseband processors share a handful of support files
 * (spectrum_collector.cpp, filtered_spectrum_collector.cpp, tv_collector.cpp,
 * ...) that call EventDispatcher::events_flag(EVT_MASK_SPECTRUM) to wake the
 * M4 event loop when a waterfall frame is ready. On the device that class is
 * the M4 dispatcher in firmware/baseband/event_m4.hpp, owned by event_m4.cpp.
 *
 * The desktop port runs the whole binary as the M0 application, which has its
 * own, unrelated class of the same name in firmware/application/event_m0.hpp
 * (owned by event_m0.cpp). Keeping both would be an ODR violation: the two
 * global classes EventDispatcher mangle to the same symbols and the linker
 * would merge event_m4's thread_event_loop and run() with event_m0's.
 *
 * So the M4 translation units see this shadow instead, force-included ahead of
 * their own `#include "event_m4.hpp"` (its include guard suppresses the real
 * one). It keeps only the surface the processors use:
 *
 *   - the EVT_MASK_* constants the collectors flag,
 *   - events_flag()/events_flag_isr(), forwarded to the host's M0 wake-up,
 *   - a constructor/run()/request_stop() so each processor's (unused, later
 *     removed) main() still compiles.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __EVENT_M4_H__
#define __EVENT_M4_H__

#include "event.hpp"

#include "baseband_thread.hpp"
#include "rssi_thread.hpp"

#include "message.hpp"

#include "ch.h"
#include "host_forced.hpp"

#include <memory>

constexpr auto EVT_MASK_BASEBAND = EVENT_MASK(0);
constexpr auto EVT_MASK_SPECTRUM = EVENT_MASK(1);

/* The shim lives in its own namespace so its member functions get distinct
 * mangled names; a global class would still merge with the M0 dispatcher's
 * inline events_flag() at link time even though only one class is visible per
 * translation unit. The using-declaration below keeps the processors' own
 * `EventDispatcher::events_flag(...)` spelling working unchanged. */
namespace host_m4_shim {

class EventDispatcher {
   public:
    explicit EventDispatcher(std::unique_ptr<BasebandProcessor> baseband_processor)
        : baseband_processor_{std::move(baseband_processor)} {}

    /* The host runs the message pump itself (host_m4 / the M0 command
     * channel), so the processor's own event loop is never entered. */
    void run() {}
    void request_stop() {}

    static void events_flag(const eventmask_t events) {
        (void)events;
        host_baseband_m4_doorbell();
    }

    static void events_flag_isr(const eventmask_t events) {
        (void)events;
        host_baseband_m4_doorbell();
    }

   private:
    std::unique_ptr<BasebandProcessor> baseband_processor_;
};

}  // namespace host_m4_shim

using host_m4_shim::EventDispatcher;

#endif /*__EVENT_M4_H__*/

/*
 * Force-included host replacement for firmware/common/lpc43xx_cpp.hpp.
 *
 * The upstream header is a thin wrapper over memory-mapped LPC43xx registers.
 * On the host we only need the types and namespace entry points that leak into
 * portable code (RTC time stamps, CREG event doorbells). The upstream header is
 * skipped by pre-defining its include guard on the compiler command line.
 */
#ifndef __HOST_FORCED_HPP__
#define __HOST_FORCED_HPP__

#include "hal.h"
#include "nvic.h"

/* Portable ARM Cortex-M4 DSP intrinsics + SIMD value types (vec2_s16/vec4_s8).
 * The baseband DSP sources (dsp_decimate, dsp_fft, ...) are written against the
 * CMSIS core intrinsics; this shim supplies them on x86-64 and pre-empts the
 * M4-only firmware/common/simd.hpp, so the unmodified DSP compiles here. */
#include "host_simd.hpp"

/* host_simd.hpp pulls in complex.hpp, which includes mathdef.hpp and thus
 * defines the bare `PI` macro in every translation unit. On the device mathdef
 * is only included where wanted (e.g. adsb.hpp, sonde_packet.cpp), so files
 * that declare their own `PI` identifier -- vdl2_decoder.cpp's
 * `constexpr float PI` -- never saw the macro. Undo the leak for the one file
 * that collides (it opts in with HOST_UNDEF_PI); every other file keeps the
 * macro, since mathdef.hpp is guarded by #pragma once and would not redefine it
 * on a later include. */
#ifdef HOST_UNDEF_PI
#ifdef PI
#undef PI
#endif
#endif

/* phase_detector.hpp (used by ook.hpp) asserts `sizeof(uint32_t) ==
 * sizeof(unsigned long)`, which holds on the 32-bit ARM M4 but not on x86-64.
 * Including the host shadow here first defines its guard, so ook.hpp's own
 * quoted include of the upstream file becomes a no-op. See
 * platform/include/phase_detector.hpp. */
#include "phase_detector.hpp"

/* The upstream LCD driver and backlight code include "portapack_io.hpp" by
 * name, which quoted-include rules would resolve to the firmware tree before
 * any -I shadow. Pre-defining the guard and force-including the host bus here
 * makes those translation units use the desktop implementation instead, while
 * staying byte-for-byte upstream. */
#define __PORTAPACK_IO_H__
#include "host_portapack_io.hpp"

/* The firmware ships its own FatFs-backed std::filesystem shim in
 * firmware/application/file.hpp. Its inline path::operator/= indexes into a
 * possibly empty string, which aborts the host process; host_file.hpp is a
 * verbatim copy with that one expression guarded. Because quoted includes
 * resolve to the includer's own directory first (most includers of "file.hpp"
 * live next to the upstream header), a shadow -I path would not be picked up,
 * so the upstream guard __FILE_H__ is pre-defined and the replacement is
 * force-included here instead.
 *
 * A couple of host-only sources (the FatFs shim and the register map) use the
 * real C++17 <filesystem> instead; they define HOST_STD_FILESYSTEM so this
 * header is skipped for them and the two std::filesystem definitions never
 * collide in one translation unit. */
#ifndef HOST_STD_FILESYSTEM
#include "host_file.hpp"
#else
#define __FILE_H__  /* keep upstream "file.hpp" out of these TUs too */
#endif

#include <cstdint>
#include <cstddef>
#include <cmath>

/* ChibiOS RTCTime lives in the HAL headers. */
struct RTCTime {
    uint32_t tv_date;
    uint32_t tv_time;
};

/* ChibiOS RTC driver shim. The implementation lives in platform/src/rtc_time.cpp. */
struct RTCDriver {
    int dummy{0};
};
extern RTCDriver RTCD1;
void rtcGetTime(RTCDriver* rtc, RTCTime* timespec);
void rtcSetTime(RTCDriver* rtc, const RTCTime* timespec);

/* On the device the M4 asserts this doorbell (SEV) when it has produced a
 * buffer or a message; the M0's M4Core_IRQHandler runs BufferExchange::handle_isr
 * and EventDispatcher::check_fifo_isr. The host emulates the M4 with its own
 * thread (platform/src/host_baseband.cpp), which calls this hook for the same
 * wake-up; defined there, declared here so the port's own baseband sources
 * (firmware/baseband/stream_input.cpp, stream_output.cpp) compile unchanged.
 *
 * Declared at global scope: an extern "C" declaration inside a namespace is
 * still looked up in that namespace, which would hide it from the M4 shadow
 * header and host_m4.cpp. */
extern "C" void host_baseband_m4_doorbell();

namespace lpc43xx {

namespace m4 {
inline bool flag_saturation() {
    return false;
}
inline void clear_flag_saturation() {}
}  // namespace m4

namespace creg {

namespace m4txevent {

inline void assert_event() { host_baseband_m4_doorbell(); }
inline void clear() {}
inline void enable() {}
inline void disable() {}

}  // namespace m4txevent

namespace m0apptxevent {
inline void clear() {}
inline void enable() {}
inline void disable() {}
inline void assert_event() {}
}  // namespace m0apptxevent

}  // namespace creg

namespace cgu {
enum class CLK_SEL {
    GP_CLKIN = 0,
    SPIFI = 1,
    M4 = 2,
    USB0 = 3,
};
}  // namespace cgu

namespace rtc {

namespace interrupt {
inline void clear_all() {}
inline void enable_second_inc() {}
}  // namespace interrupt

struct RTC : public RTCTime {
    constexpr RTC(
        uint32_t year,
        uint32_t month,
        uint32_t day,
        uint32_t hour,
        uint32_t minute,
        uint32_t second)
        : RTCTime{
              (year << 16) | (month << 8) | (day << 0),
              (hour << 16) | (minute << 8) | (second << 0)} {
    }

    constexpr RTC()
        : RTCTime{0, 0} {
    }

    /* Timestamp::now() on the M0 side is lpc43xx::rtc::RTC::now(). The device
     * reads the RTC registers; here it returns the host wall clock, packed in
     * the same BCD-free layout the struct getters below expect. */
    static RTC now();

    uint16_t year() const {
        return (tv_date >> 16) & 0xfff;
    }
    uint8_t month() const {
        return (tv_date >> 8) & 0x00f;
    }
    uint8_t day() const {
        return (tv_date >> 0) & 0x01f;
    }
    uint8_t hour() const {
        return (tv_time >> 16) & 0x01f;
    }
    uint8_t minute() const {
        return (tv_time >> 8) & 0x03f;
    }
    uint8_t second() const {
        return (tv_time >> 0) & 0x03f;
    }
};

}  // namespace rtc

}  // namespace lpc43xx

/* The firmware was written for a 32-bit target where size_t is unsigned int.
 * A few call sites mix 1U with size_t, which is ambiguous on 64-bit hosts.
 * Providing mixed-type overloads keeps the upstream code unmodified. */
#include <algorithm>
#include <type_traits>

namespace std {

template <class T, class U, enable_if_t<!is_same_v<T, U>, int> = 0>
constexpr common_type_t<T, U> max(T a, U b) {
    return (a < b) ? static_cast<common_type_t<T, U>>(b) : static_cast<common_type_t<T, U>>(a);
}

template <class T, class U, enable_if_t<!is_same_v<T, U>, int> = 0>
constexpr common_type_t<T, U> min(T a, U b) {
    return (b < a) ? static_cast<common_type_t<T, U>>(b) : static_cast<common_type_t<T, U>>(a);
}

}  // namespace std

/* Upstream calls clip(size_t, unsigned, size_t) in a few places; the original
 * template only accepts one type for all three arguments. */
template <typename T, typename U, typename V>
constexpr auto clip(const T& value, const U& minimum, const V& maximum)
    -> std::common_type_t<T, U, V> {
    using C = std::common_type_t<T, U, V>;
    const C v = static_cast<C>(value);
    const C lo = static_cast<C>(minimum);
    const C hi = static_cast<C>(maximum);
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

#endif /* __HOST_FORCED_HPP__ */

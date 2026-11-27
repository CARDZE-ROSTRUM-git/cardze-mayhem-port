/*
 * Host shadow of firmware/baseband/phase_detector.hpp.
 *
 * The upstream header is used by ook.hpp (proc_tpms / proc_ook and friends) and
 * is written for the ARM M4, where `unsigned long` is 32 bits:
 *
 *     static_assert(sizeof(history_t) == sizeof(unsigned long),
 *                   "popcountl size mismatch");
 *     __builtin_popcountl(symbol_history & late_mask);
 *
 * history_t is uint32_t. On x86-64 `unsigned long` is 64 bits, so that
 * static_assert fails to compile even though the arithmetic is identical: the
 * masked value is always within 32 bits, so popcountl and popcount agree.
 *
 * This shadow keeps the class byte-for-byte behaviourally identical and only
 * drops the ARM-width assertion, using the 32-bit primitive that matches the
 * type. It is force-included from host_forced.hpp so its include guard is set
 * before ook.hpp pulls in the upstream file by quoted include (which resolves
 * next to ook.hpp in the firmware tree and would otherwise win). Upstream is
 * left unmodified.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __PHASE_DETECTOR_HPP__
#define __PHASE_DETECTOR_HPP__

#include <cstdint>
#include <cstddef>
#include <cmath>

class PhaseDetectorEarlyLateGate {
   public:
    using history_t = uint32_t;

    using symbol_t = bool;
    using error_t = int;

    struct result_t {
        symbol_t symbol;
        error_t error;
    };

    constexpr PhaseDetectorEarlyLateGate(
        const size_t samples_per_symbol)
        : sample_threshold{samples_per_symbol / 2},
          late_mask{(1UL << sample_threshold) - 1UL},
          early_mask{late_mask << sample_threshold} {
    }

    result_t operator()(const history_t symbol_history) const {
        // history = ...0111, early
        // history = ...1110, late

        const size_t late_side = __builtin_popcount(symbol_history & late_mask);
        const size_t early_side = __builtin_popcount(symbol_history & early_mask);
        const size_t total_count = late_side + early_side;
        const auto lateness = static_cast<int>(late_side) - static_cast<int>(early_side);
        const symbol_t symbol = (total_count >= sample_threshold);
        const error_t error = symbol ? -lateness : lateness;
        return {symbol, error};
    }

   private:
    const size_t sample_threshold;
    const history_t late_mask;
    const history_t early_mask;
};

#endif /*__PHASE_DETECTOR_HPP__*/

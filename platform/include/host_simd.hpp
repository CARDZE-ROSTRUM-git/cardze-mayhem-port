/*
 * Portable host implementations of the ARM Cortex-M4 DSP intrinsics and SIMD
 * value types used by the Mayhem baseband pipeline.
 *
 * The M4 baseband DSP (dsp_decimate, dsp_fft, spectrum_collector, ...) is
 * written against the CMSIS core intrinsics: saturating maths, packed 16-bit
 * SIMD multiply-accumulate, byte/halfword extraction, bit packing and the
 * vec2_s16/vec4_s8 helpers from firmware/common/simd.hpp. Those only exist for
 * an ARM target. On the host this header re-implements every intrinsic the
 * ported sources reference, with the exact architectural semantics, in plain C.
 *
 * The result: the *unmodified* upstream DSP translation units compile and run
 * on x86-64, so the receive chain (decimation, spectrum) executes the same
 * arithmetic it does on the device.
 *
 * It is force-included for every translation unit (see host_forced.hpp) and
 * pre-defines the guard of firmware/common/simd.hpp, whose contents are M4-only,
 * so that the value types below are the ones the DSP sources see.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __HOST_SIMD_HPP__
#define __HOST_SIMD_HPP__

#include <cstdint>
#include <cstddef>

/* The upstream firmware/common/simd.hpp only defines anything under
 * LPC43XX_M4. Pre-defining its guard keeps that header out of the way so the
 * definitions below are the single source of truth on the host. */
#ifndef __SIMD_H__
#define __SIMD_H__
#endif

/* ---------------------------------------------------------------------- */
/* Generic helpers                                                        */
/* ---------------------------------------------------------------------- */

static inline uint32_t host_ror(uint32_t x, uint32_t n) {
    n &= 31u;
    return n ? ((x >> n) | (x << (32u - n))) : x;
}

static inline int32_t host_asr_i32(int32_t x, uint32_t n) {
    if (n > 31u) n = 31u;
    return x >> n; /* GCC arithmetic-shifts signed types. */
}

/* __SIMD32: upstream lpc43xx_m4.h defines it as an lvalue that aliases the
 * pointer variable itself, so '*__SIMD32(p)++' reads a word and advances p.
 * Reproduce that exactly (an rvalue cast would reject the postfix ++). */
#ifndef __SIMD32
#define __SIMD32(addr) (*(uint32_t**)&(addr))
#endif

/* ---------------------------------------------------------------------- */
/* Saturation                                                            */
/* ---------------------------------------------------------------------- */

static inline int32_t __SSAT(int32_t val, uint32_t sat) {
    if (sat >= 32u) return val;
    const int32_t hi = (int32_t)((1u << (sat - 1)) - 1u);
    const int32_t lo = -hi - 1;
    if (val > hi) return hi;
    if (val < lo) return lo;
    return val;
}

static inline uint32_t __USAT(int32_t val, uint32_t sat) {
    if (sat >= 32u) return (uint32_t)val;
    const uint32_t hi = (sat == 0u) ? 0u : ((1u << sat) - 1u);
    if (val < 0) return 0;
    if ((uint32_t)val > hi) return hi;
    return (uint32_t)val;
}

/* ---------------------------------------------------------------------- */
/* Bit manipulation                                                      */
/* ---------------------------------------------------------------------- */

static inline uint32_t __RBIT(uint32_t value) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (value & 1u);
        value >>= 1;
    }
    return r;
}

static inline uint32_t __REV16(uint32_t value) {
    return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
}

static inline uint32_t __REV(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0x0000FF00u) |
           ((value << 8) & 0x00FF0000u) | (value << 24);
}

static inline uint8_t __CLZ(uint32_t value) {
    if (value == 0) return 32;
    uint8_t n = 0;
    while ((value & 0x80000000u) == 0) {
        value <<= 1;
        n++;
    }
    return n;
}

/* __PKHBT: result[15:0] = a[15:0]; result[31:16] = (b << sh)[31:16]. */
static inline uint32_t __PKHBT(uint32_t a, uint32_t b, uint32_t sh) {
    return (a & 0x0000FFFFu) | ((b << (sh & 31u)) & 0xFFFF0000u);
}

/* __PKHTB: result[31:16] = a[31:16]; result[15:0] = (b asr sh)[15:0].
 * sh == 0 means shift by 32 (GCC inline-asm form uses asr). */
static inline uint32_t __PKHTB(uint32_t a, uint32_t b, uint32_t sh) {
    if (sh == 0u) sh = 32u;
    return (a & 0xFFFF0000u) | (((uint32_t)host_asr_i32((int32_t)b, sh)) & 0x0000FFFFu);
}

/* __BFI: insert the low `width` bits of rn into rd at bit `lsb`. */
static inline uint32_t __BFI(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width) {
    const uint32_t mask = (width >= 32u) ? 0xFFFFFFFFu : (((1u << width) - 1u) << lsb);
    return (rd & ~mask) | ((rn << lsb) & mask);
}

/* ---------------------------------------------------------------------- */
/* Halfword / byte extraction and extension                              */
/* ---------------------------------------------------------------------- */

static inline int32_t __SXTH(uint32_t x, uint32_t ror) {
    return (int32_t)(int16_t)(uint16_t)host_ror(x, ror);
}

static inline int32_t __SXTB(uint32_t x, uint32_t ror) {
    return (int32_t)(int8_t)(uint8_t)host_ror(x, ror);
}

static inline uint32_t __UXTH(uint32_t x, uint32_t ror) {
    return (uint32_t)(uint16_t)host_ror(x, ror);
}

static inline uint32_t __UXTB(uint32_t x, uint32_t ror) {
    return (uint32_t)(uint8_t)host_ror(x, ror);
}

/* Sign-extend the two low bytes into two 16-bit lanes. The upstream M4 build
 * overrides __SXTB16 with a two-argument (value, ror) form; match that. */
static inline uint32_t __SXTB16(uint32_t x, uint32_t ror) {
    const uint32_t v = host_ror(x, ror);
    const uint32_t lo = (uint32_t)(int32_t)(int8_t)(uint8_t)(v & 0xFFu);
    const uint32_t hi = (uint32_t)(int32_t)(int8_t)(uint8_t)(v >> 16);
    return (lo & 0x0000FFFFu) | ((hi & 0x0000FFFFu) << 16);
}

static inline int32_t __SXTAH(uint32_t x, uint32_t y, uint32_t ror) {
    return (int32_t)x + (int32_t)(int16_t)(uint16_t)host_ror(y, ror);
}

/* ---------------------------------------------------------------------- */
/* Signed packed multiply / multiply-accumulate                          */
/* ---------------------------------------------------------------------- */

static inline int32_t __SMULBB(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)a * (int32_t)(int16_t)(uint16_t)b;
}
static inline int32_t __SMULBT(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)a * (int32_t)(int16_t)(uint16_t)(b >> 16);
}
static inline int32_t __SMULTB(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)(a >> 16) * (int32_t)(int16_t)(uint16_t)b;
}
static inline int32_t __SMULTT(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)(a >> 16) * (int32_t)(int16_t)(uint16_t)(b >> 16);
}

static inline int32_t __SMLABB(uint32_t a, uint32_t b, int32_t c) { return c + __SMULBB(a, b); }
static inline int32_t __SMLABT(uint32_t a, uint32_t b, int32_t c) { return c + __SMULBT(a, b); }
static inline int32_t __SMLATB(uint32_t a, uint32_t b, int32_t c) { return c + __SMULTB(a, b); }
static inline int32_t __SMLATT(uint32_t a, uint32_t b, int32_t c) { return c + __SMULTT(a, b); }

/* Dual 16-bit signed multiply-add: a.lo*b.lo + a.hi*b.hi. */
static inline int32_t __SMUAD(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)a * (int32_t)(int16_t)(uint16_t)b +
           (int32_t)(int16_t)(uint16_t)(a >> 16) * (int32_t)(int16_t)(uint16_t)(b >> 16);
}

static inline int32_t __SMUADX(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)a * (int32_t)(int16_t)(uint16_t)(b >> 16) +
           (int32_t)(int16_t)(uint16_t)(a >> 16) * (int32_t)(int16_t)(uint16_t)b;
}

static inline int32_t __SMLAD(uint32_t a, uint32_t b, int32_t c) { return c + __SMUAD(a, b); }
static inline int32_t __SMLADX(uint32_t a, uint32_t b, int32_t c) { return c + __SMUADX(a, b); }

/* Dual 16-bit signed multiply-subtract: a.lo*b.lo - a.hi*b.hi. */
static inline int32_t __SMUSD(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)a * (int32_t)(int16_t)(uint16_t)b -
           (int32_t)(int16_t)(uint16_t)(a >> 16) * (int32_t)(int16_t)(uint16_t)(b >> 16);
}

static inline int32_t __SMUSDX(uint32_t a, uint32_t b) {
    return (int32_t)(int16_t)(uint16_t)a * (int32_t)(int16_t)(uint16_t)(b >> 16) -
           (int32_t)(int16_t)(uint16_t)(a >> 16) * (int32_t)(int16_t)(uint16_t)b;
}

static inline int32_t __SMLSD(uint32_t a, uint32_t b, int32_t c) { return c + __SMUSD(a, b); }
static inline int32_t __SMLSDX(uint32_t a, uint32_t b, int32_t c) { return c + __SMUSDX(a, b); }

/* 64-bit accumulating dual multiply (add / subtract variants). */
static inline int64_t __SMLALD(uint32_t a, uint32_t b, int64_t c) { return c + (int64_t)__SMUAD(a, b); }
static inline int64_t __SMLALDX(uint32_t a, uint32_t b, int64_t c) { return c + (int64_t)__SMUADX(a, b); }
static inline int64_t __SMLSLD(uint32_t a, uint32_t b, int64_t c) { return c + (int64_t)__SMUSD(a, b); }
static inline int64_t __SMLSLDX(uint32_t a, uint32_t b, int64_t c) { return c + (int64_t)__SMUSDX(a, b); }

/* Saturating 32-bit add / subtract. */
static inline int32_t __QADD(int32_t a, int32_t b) {
    const int64_t r = (int64_t)a + (int64_t)b;
    if (r > 0x7FFFFFFFLL) return (int32_t)0x7FFFFFFF;
    if (r < (int64_t)(-0x80000000LL)) return (int32_t)0x80000000;
    return (int32_t)r;
}

static inline int32_t __QSUB(int32_t a, int32_t b) {
    const int64_t r = (int64_t)a - (int64_t)b;
    if (r > 0x7FFFFFFFLL) return (int32_t)0x7FFFFFFF;
    if (r < (int64_t)(-0x80000000LL)) return (int32_t)0x80000000;
    return (int32_t)r;
}

/* Per-lane saturating 16-bit add / subtract (packed in a 32-bit word). */
static inline uint32_t __QADD16(uint32_t a, uint32_t b) {
    const int32_t lo = __SSAT((int32_t)(int16_t)(uint16_t)a + (int32_t)(int16_t)(uint16_t)b, 16);
    const int32_t hi = __SSAT((int32_t)(int16_t)(uint16_t)(a >> 16) + (int32_t)(int16_t)(uint16_t)(b >> 16), 16);
    return ((uint32_t)lo & 0xFFFFu) | (((uint32_t)hi & 0xFFFFu) << 16);
}

static inline uint32_t __QSUB16(uint32_t a, uint32_t b) {
    const int32_t lo = __SSAT((int32_t)(int16_t)(uint16_t)a - (int32_t)(int16_t)(uint16_t)b, 16);
    const int32_t hi = __SSAT((int32_t)(int16_t)(uint16_t)(a >> 16) - (int32_t)(int16_t)(uint16_t)(b >> 16), 16);
    return ((uint32_t)lo & 0xFFFFu) | (((uint32_t)hi & 0xFFFFu) << 16);
}

/* ---------------------------------------------------------------------- */
/* 32x32 -> top 32 bits                                                   */
/* ---------------------------------------------------------------------- */

static inline int32_t __SMMUL(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}

static inline int32_t __SMMULR(int32_t a, int32_t b) {
    return (int32_t)((((int64_t)a * (int64_t)b) + 0x80000000LL) >> 32);
}

/* ---------------------------------------------------------------------- */
/* SIMD value types (firmware/common/simd.hpp equivalents)                */
/* ---------------------------------------------------------------------- */

struct vec4_s8 {
    union {
        int8_t v[4];
        uint32_t w;
    };
};

struct vec2_s16 {
    constexpr vec2_s16()
        : v{0, 0} {
    }

    constexpr vec2_s16(const int16_t v)
        : v{v, v} {
    }

    constexpr vec2_s16(const int16_t v0, const int16_t v1)
        : v{v0, v1} {
    }

    constexpr vec2_s16(const vec2_s16& other)
        : w{other.w} {
    }

    vec2_s16& operator=(const vec2_s16& other) {
        w = other.w;
        return *this;
    }

    union {
        int16_t v[2];
        uint32_t w;
    };
};

static inline vec4_s8 rev16(const vec4_s8 v) {
    vec4_s8 result;
    result.w = __REV16(v.w);
    return result;
}

static inline vec4_s8 pkhbt(const vec4_s8 v1, const vec4_s8 v2, const size_t sh = 0) {
    vec4_s8 result;
    result.w = __PKHBT(v1.w, v2.w, sh);
    return result;
}

static inline vec2_s16 pkhbt(const vec2_s16 v1, const vec2_s16 v2, const size_t sh = 0) {
    vec2_s16 result;
    result.w = __PKHBT(v1.w, v2.w, sh);
    return result;
}

static inline vec2_s16 pkhtb(const vec2_s16 v1, const vec2_s16 v2, const size_t sh = 0) {
    vec2_s16 result;
    result.w = __PKHTB(v1.w, v2.w, sh);
    return result;
}

static inline vec2_s16 sxtb16(const vec4_s8 v, const size_t sh = 0) {
    vec2_s16 result;
    result.w = __SXTB16(v.w, sh);
    return result;
}

static inline int32_t smlsd(const vec2_s16 v1, const vec2_s16 v2, const int32_t accum) {
    return __SMLSD(v1.w, v2.w, accum);
}

static inline int32_t smlad(const vec2_s16 v1, const vec2_s16 v2, const int32_t accum) {
    return __SMLAD(v1.w, v2.w, accum);
}

/* The firmware keeps multiply_conjugate_s16_s32 in common/utility_m4.hpp, which
 * only defines it under LPC43XX_M4. The host DSP (dsp_demodulate) needs the same
 * conjugate-multiply; provide it here with the identical arithmetic. complex.hpp
 * supplies complex16_t/complex32_t. */
#if defined(__cplusplus)
#include "complex.hpp"

static inline complex32_t multiply_conjugate_s16_s32(
    const complex16_t::rep_type a,
    const complex16_t::rep_type b) {
    const int32_t rr = __SMULBB(a, b);
    const int32_t ii = __SMULTT(a, b);
    const int32_t r = __QADD(rr, ii);
    const int32_t ir = __SMULTB(a, b);
    const int32_t ri = __SMULBT(a, b);
    const int32_t i = __QSUB(ir, ri);
    return {r, i};
}
#endif

#endif /* __HOST_SIMD_HPP__ */

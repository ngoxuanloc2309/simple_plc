/*
 * plc_math.c - Layer 2 (PLC Core): ALU of the MATH rule block, see plc_math.h.
 *
 * Rules (SPLC-SPEC-MATH-007 section 4):
 *   int32 : ADD/SUB wrap mod 2^32; MUL clamps to [INT32_MIN, INT32_MAX]; DIV/MOD by 0 = fault;
 *           INT32_MIN / -1 = write INT32_MAX + fault; INT32_MIN % -1 = 0, no fault; C99
 *           truncating division, remainder takes the sign of the dividend.
 *   float : IEEE-754 single; divisor compared with EXACT zero (+0.0 and -0.0); a NaN or
 *           +-Inf result (overflow, Inf-Inf, NaN input, Inf mod y) is a fault.
 *   fault : the output tag keeps its old value (except the INT32_MIN / -1 case) and
 *           health bit 3 is raised for SPLC_MATH_FAULT_HOLD_MS.
 */

#include "plc_math.h"

#if SPLC_MATH_ENABLE

#include <limits.h>

/* ---------------------------------------------------------------------------------------- */
/* float32 helpers on raw bits (no isnan()/isinf(): they must not depend on -ffast-math)   */
/* ---------------------------------------------------------------------------------------- */

static inline float bits_f(uint32_t b) { union { uint32_t u; float f; } c; c.u = b; return c.f; }
static inline uint32_t f_bits(float f) { union { uint32_t u; float f; } c; c.f = f; return c.u; }

#define F32_EXP_MASK   0x7F800000UL
#define F32_SIGN_MASK  0x80000000UL
#define F32_NAN        0x7FC00000UL

static inline bool f32_is_finite(uint32_t b) { return (b & F32_EXP_MASK) != F32_EXP_MASK; }
static inline bool f32_is_zero(uint32_t b)   { return (b << 1) == 0u; }   /* +0.0 and -0.0 */

/*
 * splc_fmodf: exact fmod for float32 (the result is representable, so no rounding is involved
 * and every correct implementation returns the same bits). Shift-and-subtract long division on
 * the mantissas, after the algorithm of musl libc's fmodf.
 *
 *   Copyright (c) 2005-2020 Rich Felker, et al.
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this
 *   software and associated documentation files (the "Software"), to deal in the Software
 *   without restriction, including without limitation the rights to use, copy, modify, merge,
 *   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
 *   to whom the Software is furnished to do so, subject to the following conditions:
 *   The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 *   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 *   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 *   FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 *
 * Works on bits and returns bits. A zero divisor, NaN, or an Inf dividend returns a quiet NaN
 * bit pattern (the caller turns any non-finite result into a fault).
 */
static uint32_t splc_fmodf_bits(uint32_t uxi, uint32_t uyi)
{
    int      ex = (int)((uxi >> 23) & 0xFFU);
    int      ey = (int)((uyi >> 23) & 0xFFU);
    uint32_t sx = uxi & F32_SIGN_MASK;
    uint32_t i;

    if ((uyi << 1) == 0u || (uyi << 1) > 0xFF000000UL || ex == 0xFF) {
        return F32_NAN;                    /* y == 0, y NaN, x Inf or NaN */
    }
    if ((uxi << 1) <= (uyi << 1)) {
        if ((uxi << 1) == (uyi << 1)) {
            return sx;                     /* |x| == |y| -> +-0 */
        }
        return uxi;                        /* |x| < |y| -> x */
    }

    /* normalize x and y */
    if (ex == 0) {
        for (i = uxi << 9; (i >> 31) == 0u; ex--, i <<= 1) { }
        uxi <<= (unsigned)(-ex + 1);
    } else {
        uxi &= 0xFFFFFFFFUL >> 9;
        uxi |= 1UL << 23;
    }
    if (ey == 0) {
        for (i = uyi << 9; (i >> 31) == 0u; ey--, i <<= 1) { }
        uyi <<= (unsigned)(-ey + 1);
    } else {
        uyi &= 0xFFFFFFFFUL >> 9;
        uyi |= 1UL << 23;
    }

    /* x mod y */
    for (; ex > ey; ex--) {
        i = uxi - uyi;
        if ((i >> 31) == 0u) {
            if (i == 0u) {
                return sx;
            }
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uyi;
    if ((i >> 31) == 0u) {
        if (i == 0u) {
            return sx;
        }
        uxi = i;
    }
    for (; (uxi >> 23) == 0u; uxi <<= 1, ex--) { }

    /* scale result up */
    if (ex > 0) {
        uxi -= 1UL << 23;
        uxi |= (uint32_t)ex << 23;
    } else {
        uxi >>= (unsigned)(-ex + 1);
    }
    return uxi | sx;
}

/* ---------------------------------------------------------------------------------------- */

SPLC_MathResult splc_math_i32(uint8_t op, int32_t a, int32_t b, int32_t *out)
{
    switch (op) {
    case SPLC_MATH_ADD:
        *out = (int32_t)((uint32_t)a + (uint32_t)b);
        return SPLC_MATH_OK;
    case SPLC_MATH_SUB:
        *out = (int32_t)((uint32_t)a - (uint32_t)b);
        return SPLC_MATH_OK;
    case SPLC_MATH_MUL: {
        int64_t p = (int64_t)a * (int64_t)b;
        if (p > (int64_t)INT32_MAX)      { p = INT32_MAX; }
        else if (p < (int64_t)INT32_MIN) { p = INT32_MIN; }
        *out = (int32_t)p;
        return SPLC_MATH_OK;
    }
    case SPLC_MATH_DIV:
        if (b == 0) {
            return SPLC_MATH_FAULT_KEEP;
        }
        if (a == INT32_MIN && b == -1) {
            *out = INT32_MAX;
            return SPLC_MATH_FAULT_SAT;
        }
        *out = a / b;
        return SPLC_MATH_OK;
    case SPLC_MATH_MOD:
        if (b == 0) {
            return SPLC_MATH_FAULT_KEEP;
        }
        if (b == -1) {
            *out = 0;                       /* also INT32_MIN % -1: defined as 0, no fault */
            return SPLC_MATH_OK;
        }
        *out = a % b;
        return SPLC_MATH_OK;
    default:
        return SPLC_MATH_FAULT_KEEP;
    }
}

SPLC_MathResult splc_math_f32(uint8_t op, uint32_t a_bits, uint32_t b_bits, uint32_t *out_bits)
{
    uint32_t r;

    if ((op == SPLC_MATH_DIV || op == SPLC_MATH_MOD) && f32_is_zero(b_bits)) {
        return SPLC_MATH_FAULT_KEEP;
    }

    /* Each result goes through a volatile so the compiler computes exactly one IEEE operation
     * (no contraction, no folding), the same way action_scale_tag() does. */
    volatile float v;
    const float a = bits_f(a_bits);
    const float b = bits_f(b_bits);
    switch (op) {
    case SPLC_MATH_ADD: v = a + b; r = f_bits(v); break;
    case SPLC_MATH_SUB: v = a - b; r = f_bits(v); break;
    case SPLC_MATH_MUL: v = a * b; r = f_bits(v); break;
    case SPLC_MATH_DIV: v = a / b; r = f_bits(v); break;
    case SPLC_MATH_MOD: r = splc_fmodf_bits(a_bits, b_bits); break;
    default:            return SPLC_MATH_FAULT_KEEP;
    }

    if (!f32_is_finite(r)) {
        return SPLC_MATH_FAULT_KEEP;       /* NaN or +-Inf */
    }
    *out_bits = r;
    return SPLC_MATH_OK;
}

/* ---------------------------------------------------------------------------------------- */

static bool     s_fault_seen;
static uint32_t s_fault_ms;

void splc_math_fault_note(uint32_t now_ms)
{
    s_fault_ms   = now_ms;
    s_fault_seen = true;
}

bool splc_math_fault_active(uint32_t now_ms)
{
    if (!s_fault_seen) {
        return false;
    }
    if ((uint32_t)(now_ms - s_fault_ms) < (uint32_t)SPLC_MATH_FAULT_HOLD_MS) {
        return true;
    }
    s_fault_seen = false;
    return false;
}

void splc_math_fault_clear(void)
{
    s_fault_seen = false;
    s_fault_ms   = 0u;
}

#endif /* SPLC_MATH_ENABLE */
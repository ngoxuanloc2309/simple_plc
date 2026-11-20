#ifndef PLC_MATH_H
#define PLC_MATH_H

/*
 * plc_math.h - Layer 2 (PLC Core)
 *
 * ALU of the MATH rule block (action_type = 8, SPLC-SPEC-MATH-007 sections 3..4):
 *   OUT = Op1 <op> Op2   on int32 or float32, every scan, no state.
 * Pure functions plus one tiny piece of state (the time of the last fault, for health bit 3).
 * No Layer 0/1 include, no heap, no libm, no CPU divide-by-zero trap (the divisor is tested
 * before every divide). Compiled only with SPLC_MATH_ENABLE=1.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#ifdef __cplusplus
extern "C" {
#endif

#if SPLC_MATH_ENABLE

/* Operator codes = rule compare_op (wire reg 12 high byte). */
typedef enum {
    SPLC_MATH_ADD = 0,
    SPLC_MATH_SUB = 1,
    SPLC_MATH_MUL = 2,
    SPLC_MATH_DIV = 3,
    SPLC_MATH_MOD = 4,
    SPLC_MATH_OP_COUNT = 5
} SPLC_MathOp;

typedef enum {
    SPLC_MATH_OK         = 0,  /* *out holds the result, write it                                    */
    SPLC_MATH_FAULT_KEEP = 1,  /* fault: do NOT touch the output tag (keep the old value)            */
    SPLC_MATH_FAULT_SAT  = 2   /* fault, but *out = INT32_MAX must be written (INT32_MIN / -1)       */
} SPLC_MathResult;

/* int32 ALU. `op` outside 0..4 is SPLC_MATH_FAULT_KEEP. *out is only valid for OK / FAULT_SAT. */
SPLC_MathResult splc_math_i32(uint8_t op, int32_t a, int32_t b, int32_t *out);

/* float32 ALU on bit patterns (the tag slots hold float32 bits). NaN / +-Inf result, or a zero
 * divisor (+0.0 and -0.0 alike), is SPLC_MATH_FAULT_KEEP. */
SPLC_MathResult splc_math_f32(uint8_t op, uint32_t a_bits, uint32_t b_bits, uint32_t *out_bits);

/* Remember a MATH fault at `now_ms` (rule_scan's tick). */
void splc_math_fault_note(uint32_t now_ms);

/* True while the last fault is younger than SPLC_MATH_FAULT_HOLD_MS (health bit 3, 0x0803).
 * Unsigned tick arithmetic, so it survives the 49.7 day wrap. Self-clearing. */
bool splc_math_fault_active(uint32_t now_ms);

/* Forget the fault (boot / tests). */
void splc_math_fault_clear(void);

#endif /* SPLC_MATH_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* PLC_MATH_H */
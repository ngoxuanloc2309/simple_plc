#!/usr/bin/env python3
"""
test_math.py - Verify the MATH rule block (action_type = 8) of SimplePLC over Modbus
(SPLC-SPEC-MATH-007, docs/SPLC_SPEC_MATH007.md; handoff decision list item 101).

Requires: pip install pymodbus pyserial
Firmware must be built with SPLC_MATH_ENABLE=1 (needs SPLC_REAL_ENABLE=1).
Standalone like test_rule_float.py, whose helpers it imports from the same folder:
test_mqtt (Dev, commit(), run_syscmd(), register constants) and test_tagtype
(tag_value(), wait_diag_state()).

WARNING: this test WRITES Flash. It replaces the rule table about 20 times (one COMMIT per
operator/type group, plus the verbatim, SCALE_TAG, system-tag, timing and reject steps) and
ends with CLEAR_RULES, which ALSO wipes the MQTT / FB / MB config an App deployed. It enters
DIAG repeatedly to write tag values. Run it on a dev board only. The start state is made
deterministic first (CLEAR_RULES, engine running, health bit 3 clear: handoff lesson 3).

What is checked (spec section in brackets):
  0. 0x0A08 bit 16 (0x00010000) = MATH and bit 0x0800 = FLOAT32 are set; 0x0803 is readable.   [6]
  1. Verbatim storage [2]: Reg 13 LOW byte and Reg 14..15 of every rule (MATH or not, enabled or
     not) go through staging (read back at 0x9010), the CRC, COMMIT, the Active Rule Table
     (0x0100) and, with --reboot, Flash, byte-exact. The same rule set also runs: constant Op2
     (K-01..K-03, spec 8.3, Reg 14 = HIGH word), a disabled MATH rule is never executed, Reg 6..7
     of a MATH rule are ignored (C-12), a non-MATH rule next to MATH rules is unaffected.
  2. Vectors on the real engine [4, 8.1, 8.2]: INT32 I-01..I-16 and REAL R-01..R-14 (takes about
     2-3 minutes: after a faulting vector the script waits for bit 3 to fall, ~hold ms, before the
     next faulting vector, so that each vector must raise bit 3 on its own). Each
     operator/type is a one-rule COMMIT (Op1 = X, Op2 = Y tags, OUT = Z) so that health bit 3 is
     attributable. Per vector: OUT after the run and health bit 3 (0 = no fault, 1 = fault). A
     faulting vector must LEAVE OUT at its previous value (777 / 0x44424000), except
     INT32_MIN / -1 which writes INT32_MAX.
  3. System tags as operands [3.6]: X + SYS_ON (1001) and X / SYS_OFF (1002) (own additions,
     not in spec 8).
  4. Health bit 3 timing [6]: not set without a fault; set while a fault persists (also beyond
     the hold time, so it is refreshed, not a one-shot); after the last fault it falls by itself
     about --hold-ms later (default 3000, SPLC_MATH_FAULT_HOLD_MS) and does not come back.
  5. SCALE_TAG flags [7, 8.4]: 0x07 (S-01..S-03: INT32 source -> float32), 0x03 still takes the
     source as float32 bits, 0x05 / 0x06 (bit 2 without both float bits) run the INTEGER formula.
  6. COMMIT rejections [5, 8.5]: C-01..C-10 (+ a few siblings) are refused with status 4 /
     error 2, the active version does not move, the running table is unchanged; C-11 (disabled
     rule) and C-12 (Reg 6..7 set) are accepted.
  7. (--reboot) the verbatim rule set survives REBOOT byte-exact and still runs.

NOT checked here: SCALE_TAG on MATH=0 builds, time spent in splc_fmodf, the wire-level CRC
mismatch of a pre-MATH firmware, behaviour while the engine is held by DIAG beyond the
enter/exit used for stimuli.

Layout used (dense layout, tags read from DeviceResourceInfo; needs >= 12 VREG):
  vr0 = first VREG index = DI + DO + AI + VFLAG.   Groups use X = vr0, Y = vr0+1, Z = vr0+2
  (W = vr0+3 in the system-tag step); the verbatim and SCALE steps use vr0 .. vr0+11.

Usage:
  python test_math.py COM17 --unit 1 --baudrate 115200
  python test_math.py COM17 --reboot [--port-after COM18] [--hold-ms 3000]
  python test_math.py --check-vectors          # NO board: vector table vs a Python model

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error (inconclusive).
"""
import argparse
import math
import struct
import sys
import time

import test_report as R   # Excel evidence report (--report / --no-report)
import test_mqtt as T
import test_tagtype as TT
from test_mqtt import check, info, LinkError

# ------------------------------------------------------------------ constants
REG_HEALTH_FLAGS = 0x0803           # DEVICE_HEALTH.health_flags, bit 3 = MATH_FAULT
REG_CAPS = 0x0A08                   # HARDWARE_CAPABILITIES, uint32, high word first
CAP_MATH = 0x00010000
CAP_FLOAT32 = 0x0800
REG_SYSTAG = 0x0820                 # 0x0820 SYS_FIRST_SCAN, 0x0821 SYS_ON, 0x0822 SYS_OFF
ACT_SCALE, ACT_MATH = 7, 8
OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD = range(5)
OPNAME = {OP_ADD: "ADD", OP_SUB: "SUB", OP_MUL: "MUL", OP_DIV: "DIV", OP_MOD: "MOD"}
FLAGS_INT, FLAGS_REAL = 0x00, 0x03
CONST = 0x7FFF                      # guard_tag value meaning "Op2 is the constant in Reg 14..15"
ST_ERROR = 4                        # CONFIG_STATUS_ERROR
ERR_INVALID = 2                     # CONFIG_ERROR_CODE INVALID_PARAMETER
SYS_ON, SYS_OFF = 1001, 1002
SETTLE_S = 0.25                     # engine scan is 10 ms; leave margin for the link + diag exit
INT_MIN, INT_MAX = -2147483648, 2147483647
ONE_F = 0x3F800000                  # 1.0f: a safe pair (1,1 as INT or REAL) for every operator
PRE_INT, PRE_REAL = 777, 0x44424000  # OUT before the run: 777 / 777.0f


def u32v(x):
    return x & 0xFFFFFFFF


def s32(u):
    u &= 0xFFFFFFFF
    return u - (1 << 32) if u & 0x80000000 else u


# ------------------------------------------------------------------ float32 / reference model
def f32_from_bits(u):
    return struct.unpack(">f", struct.pack(">I", u & 0xFFFFFFFF))[0]


def bits_from_f32(x):
    """float32 bits of a Python float (round to nearest even, overflow -> +-Inf, NaN -> 7FC00000)."""
    if math.isnan(x):
        return 0x7FC00000
    try:
        return struct.unpack(">I", struct.pack(">f", x))[0]
    except OverflowError:
        return 0xFF800000 if x < 0 else 0x7F800000


def ref_int(op, a, b):
    """Spec section 4, INT32. a, b signed. Returns (out_or_None, fault); None = keep OUT."""
    if op == OP_ADD:
        return s32(a + b), False
    if op == OP_SUB:
        return s32(a - b), False
    if op == OP_MUL:
        return max(INT_MIN, min(INT_MAX, a * b)), False
    if b == 0:
        return None, True
    if op == OP_DIV:
        if a == INT_MIN and b == -1:
            return INT_MAX, True
        q = abs(a) // abs(b)
        return (q if (a >= 0) == (b >= 0) else -q), False
    # MOD: C99, sign of the dividend; INT_MIN % -1 = 0 and is NOT a fault
    r = abs(a) % abs(b)
    return (r if a >= 0 else -r), False


def ref_real(op, ab, bb):
    """Spec section 4, REAL. ab, bb are float32 bit patterns. Returns (out_bits_or_None, fault)."""
    a, b = f32_from_bits(ab), f32_from_bits(bb)
    if op in (OP_DIV, OP_MOD) and b == 0.0:             # +0.0 and -0.0
        return None, True
    try:
        if op == OP_ADD:
            r = a + b
        elif op == OP_SUB:
            r = a - b
        elif op == OP_MUL:
            r = a * b
        elif op == OP_DIV:
            r = a / b
        else:
            r = math.fmod(a, b)                          # exact; sign of the dividend
    except (ValueError, OverflowError, ZeroDivisionError):
        return None, True
    out = bits_from_f32(r)
    if (out & 0x7F800000) == 0x7F800000:                 # NaN or +-Inf
        return None, True
    return out, False


# ------------------------------------------------------------------ vector tables (spec section 8)
# (id, op1, op2, expected OUT or None = unchanged, fault, note)   INT values signed, REAL values = bits
INT_GROUPS = [
    (OP_ADD, [("I-01", 100, 23, 123, 0, ""),
              ("I-02", 2000000000, 2000000000, -294967296, 0, "wraps mod 2^32 (0xEE6B2800)")]),
    (OP_SUB, [("I-03", -2000000000, 2000000000, 294967296, 0, "wraps mod 2^32"),
              ("I-04", 5, 8, -3, 0, "negative result")]),
    (OP_MUL, [("I-05", 6, 7, 42, 0, ""),
              ("I-06", 1000000000, 10, INT_MAX, 0, "clamps to INT32_MAX"),
              ("I-07", -1000000000, 10, INT_MIN, 0, "clamps to INT32_MIN")]),
    (OP_DIV, [("I-08", 7, 2, 3, 0, "truncates toward 0"),
              ("I-09", -7, 2, -3, 0, "truncates toward 0 (C99)"),
              ("I-10", 5, 0, None, 1, "divisor tag = 0: OUT kept, bit 3"),
              ("I-11", INT_MIN, -1, INT_MAX, 1, "writes INT32_MAX and sets bit 3")]),
    (OP_MOD, [("I-12", 7, 3, 1, 0, ""),
              ("I-13", -7, 3, -1, 0, "sign of the dividend (C99)"),
              ("I-14", 7, -3, 1, 0, "sign of the dividend (C99)"),
              ("I-15", INT_MIN, -1, 0, 0, "result 0, NOT a fault"),
              ("I-16", 5, 0, None, 1, "divisor tag = 0: OUT kept, bit 3")]),
]
REAL_GROUPS = [
    (OP_ADD, [("R-01", 0x3FC00000, 0x40100000, 0x40700000, 0, "1.5 + 2.25 = 3.75"),
              ("R-09", 0x7F800000, 0xFF800000, None, 1, "+Inf + -Inf = NaN: OUT kept, bit 3"),
              ("R-10", 0x7FC00000, 0x3F800000, None, 1, "NaN input: OUT kept, bit 3")]),
    (OP_SUB, [("R-02", 0x41200000, 0x3E800000, 0x411C0000, 0, "10 - 0.25 = 9.75")]),
    (OP_MUL, [("R-03", 0x41BC0000, 0x40000000, 0x423C0000, 0, "23.5 * 2 = 47"),
              ("R-07", 0x7149F2CA, 0x7149F2CA, None, 1, "1e30 * 1e30 = +Inf: OUT kept, bit 3")]),
    (OP_DIV, [("R-04", 0x3F800000, 0x40400000, 0x3EAAAAAB, 0, "1 / 3 rounded to float32"),
              ("R-05", 0x3F800000, 0x00000000, None, 1, "divisor +0.0: OUT kept, bit 3"),
              ("R-06", 0x3F800000, 0x80000000, None, 1, "divisor -0.0: OUT kept, bit 3"),
              ("R-08", 0x7149F2CA, 0x0DA24260, None, 1, "1e30 / 1e-30 = +Inf: OUT kept, bit 3")]),
    (OP_MOD, [("R-11", 0x40F00000, 0x40000000, 0x3FC00000, 0, "fmod(7.5, 2) = 1.5"),
              ("R-12", 0xC0F00000, 0x40000000, 0xBFC00000, 0, "sign of the dividend"),
              ("R-13", 0x40A00000, 0x00000000, None, 1, "divisor 0: OUT kept, bit 3"),
              ("R-14", 0x7F800000, 0x40000000, None, 1, "Inf mod 2 = NaN: OUT kept, bit 3")]),
]
# Run order: fault-free groups first, so that waiting for bit 3 to fall is only needed afterwards.
GROUP_ORDER = [("INT", OP_ADD), ("INT", OP_SUB), ("INT", OP_MUL), ("REAL", OP_SUB),
               ("INT", OP_DIV), ("INT", OP_MOD), ("REAL", OP_ADD), ("REAL", OP_MUL),
               ("REAL", OP_DIV), ("REAL", OP_MOD)]


def groups():
    """[(kind, op, flags, preset, [vectors])] in run order."""
    table = {("INT", op): v for op, v in INT_GROUPS}
    table.update({("REAL", op): v for op, v in REAL_GROUPS})
    return [(k, op, FLAGS_INT if k == "INT" else FLAGS_REAL,
             PRE_INT if k == "INT" else PRE_REAL, table[(k, op)]) for k, op in GROUP_ORDER]


def check_vectors():
    """Offline: every table row against the Python reference model. No board needed."""
    bad = 0
    for kind, op, flags, preset, vecs in groups():
        for vid, a, b, out, fault, _ in vecs:
            if kind == "INT":
                got, gf = ref_int(op, a, b)
            else:
                got, gf = ref_real(op, a, b)
            ok = (got == out) and (gf == bool(fault))
            if not ok:
                bad += 1
            check(ok, f"{vid} {kind} {OPNAME[op]}: table out={out!r} fault={fault}  model out={got!r} fault={int(gf)}")
    n = sum(len(g[4]) for g in groups())
    print(f"\n{n} vectors compared with the Python model (INT: spec section 4 rules, REAL: float32 via struct/fmod)")
    return bad == 0


# ------------------------------------------------------------------ encoding
def math_rule(op1, op2, out, op, flags, const=None, reg13_low=0, param=0, enabled=1, compare_op=None):
    """16-register wire image of a MATH rule. op2 = tag index, or None to use the constant `const`
    (u32 bits) in Reg 14..15 with guard_tag = 0x7FFF. `compare_op` overrides the operator (invalid
    values for the COMMIT tests)."""
    guard = CONST if op2 is None else op2 & 0xFFFF
    aux = u32v(const) if const is not None else 0
    cop = op if compare_op is None else compare_op
    return [0, 0, 0, 0, 0, 0, *T.u32(param),
            op1 & 0xFFFF, out & 0xFFFF, guard,
            (enabled << 8) | 0, (cop << 8) | ACT_MATH,
            ((flags & 0xFF) << 8) | (reg13_low & 0xFF), (aux >> 16) & 0xFFFF, aux & 0xFFFF]


def scale_rule(src, dst, flags, gain, offset):
    """SCALE_TAG (action 7): gain = Reg 6..7, offset = Reg 2..3 (threshold_hi), flags = Reg 13 high."""
    return [*T.u32(0), *T.u32(offset), *T.u32(0), *T.u32(gain),
            src & 0xFFFF, dst & 0xFFFF, T.GUARD_NONE, (1 << 8) | 0, (0 << 8) | ACT_SCALE,
            (flags & 0xFF) << 8, 0, 0]


def set_rule(trig, out, lo, param, reg13_low=0, aux=0):
    """Non-MATH SET_TAG rule `trig > lo -> out = param` carrying junk in Reg 13 low / Reg 14..15."""
    return [*T.u32(lo), *T.u32(0), *T.u32(0), *T.u32(param), trig, out, T.GUARD_NONE,
            (1 << 8) | 0, (3 << 8) | 0, reg13_low & 0xFF, (aux >> 16) & 0xFFFF, aux & 0xFFFF]


# ------------------------------------------------------------------ device access
def read_active(d, n):
    out = []
    for i in range(n):
        out += d.read(0x0100 + 16 * i, 16)
    return out


def read_staging(d, n):
    out = []
    for i in range(n):
        out += d.read(T.REG_STAGING + 16 * i, 16)
    return out


def flat(rules):
    return [w for r in rules for w in r]


def stage(d, rules):
    d.fc06(T.REG_RULE_COUNT_STAGED, len(rules))
    for i, rule in enumerate(rules):
        d.fc16(T.REG_STAGING + i * 16, rule)


def commit_raw(d, rules):
    """Stage + COMMIT with the CORRECT CRC but without T.commit()'s retry logic; returns
    (status, error). Used for the rejection steps (T.commit would wait for READY)."""
    crc = T.crc16_modbus(b"".join(T.rule_bytes(r) for r in rules))
    stage(d, rules)
    d.fc06(T.REG_EXPECTED_CRC, crc)
    d.fc06(T.REG_COMMIT, T.COMMIT_MAGIC, safe=False)
    time.sleep(0.3)
    st, er = d.read(T.REG_STATUS, 2)
    t0 = time.time()
    while st in (1, 2) and er == 0 and time.time() - t0 < 3.0:
        time.sleep(0.1)
        st, er = d.read(T.REG_STATUS, 2)
    return st, er


def deploy(d, rules, what):
    s, e = T.commit(d, rules)
    return check(s == T.CFG_READY and e == 0, f"{what}: COMMIT ok (status {s}, error {e})")


def health_bit3(d):
    return (d.read(REG_HEALTH_FLAGS, 1)[0] >> 3) & 1


def diag_set(d, values):
    """ENTER DIAG, write {tag: uint32}, EXIT. Quiet unless something fails. The EXIT resets the
    runtime state of every rule; MATH is stateless so the next scan simply computes."""
    d.fc06(T.REG_DIAG, T.CMD_ENTER)
    if not TT.wait_diag_state(d, TT.ST_DIAG):
        return check(False, "ENTER_DIAG reached DIAG_CONTROL")
    ok = True
    for tag, v in values.items():
        resp = d.fc16(T.REG_TAGS + 2 * tag, [(v >> 16) & 0xFFFF, v & 0xFFFF])
        if resp.isError():
            ok = check(False, f"diag write tag {tag} = 0x{v:08X} accepted")
    d.fc06(T.REG_DIAG, T.CMD_EXIT)
    if not TT.wait_diag_state(d, TT.ST_ENGINE):
        ok = check(False, "EXIT_DIAG back to ENGINE_RUNNING")
    return ok


def stimulate(d, values, tags_out):
    """Write inputs/presets via DIAG, let the engine run, return ({tag: value}, health bit 3)."""
    if not diag_set(d, values):
        return None, None
    time.sleep(SETTLE_S)
    return {t: TT.tag_value(d, t) for t in tags_out}, health_bit3(d)


def wait_fault_clear(d, hold_s, what):
    """Poll bit 3 until it is 0 (it falls ~hold after the last fault). Returns True when clear."""
    t0 = time.time()
    while time.time() - t0 < hold_s + 1.5:
        if health_bit3(d) == 0:
            return True
        time.sleep(0.1)
    return check(False, f"{what}: health bit 3 fell within {hold_s + 1.5:.1f} s although no fault is active")


def fmt(kind, v):
    return f"{s32(v)}" if kind == "INT" else f"0x{v & 0xFFFFFFFF:08X}"


# ------------------------------------------------------------------ steps
def step_verbatim(d, vr0, label, stage_check=True):
    """Rule set with Reg 13 low byte / Reg 14..15 on every kind of rule; see the module docstring."""
    t = lambda i: vr0 + i
    rules = [
        math_rule(t(0), None, t(1), OP_ADD, FLAGS_INT, const=5, reg13_low=0xA5),          # K-01, low byte set
        math_rule(t(2), None, t(3), OP_MUL, FLAGS_REAL, const=0x40200000),                # K-02 4.0 * 2.5
        math_rule(t(4), None, t(5), OP_SUB, FLAGS_INT, const=0xFFFFFFFF),                 # K-03 0 - (-1)
        set_rule(t(6), t(7), 100, 5, reg13_low=0x5A, aux=0xCAFEBABE),                     # non-MATH + junk
        math_rule(t(8), None, t(9), OP_MUL, FLAGS_INT, const=0x00010002, param=0x11223344),  # C-12 + swap probe
        math_rule(t(10), None, t(11), OP_ADD, FLAGS_INT, const=0x00000001, enabled=0,
                  compare_op=5, reg13_low=0x01),                                          # C-11: disabled, op 5
    ]
    rules[5][13] = 0x0100 | 0x01          # flags = 0x01 on a DISABLED rule (C-11), low byte 0x01
    n = len(rules)
    want = flat(rules)
    print(f"\n{label}: verbatim Reg 13 low byte / Reg 14..15, constants, disabled rule")
    if stage_check:
        stage(d, rules)
        stg = read_staging(d, n)
        check(stg == want, "staging reads back byte-exact before COMMIT"
              + ("" if stg == want else f" {diff_words(stg, want)}"))
    if not deploy(d, rules, "verbatim set"):
        return None
    return rules


def diff_words(a, b):
    bad = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    return f"(first diffs: " + ", ".join(f"reg {i // 16}.{i % 16}: got 0x{a[i]:04X} want 0x{b[i]:04X}"
                                         for i in bad[:6]) + ")"


def verify_verbatim_active(d, rules, label):
    n = len(rules)
    act = read_active(d, n)
    want = flat(rules)
    check(act == want, f"{label}: Active Rule Table reads back byte-exact (Reg 13 low byte, Reg 14..15 of all {n} rules)"
          + ("" if act == want else f" {diff_words(act, want)}"))


def run_verbatim(d, vr0, label):
    """Stimulate the verbatim rule set and check the outputs."""
    t = lambda i: vr0 + i
    vals = {t(0): 10, t(1): PRE_INT, t(2): 0x40800000, t(3): PRE_REAL, t(4): 0, t(5): PRE_INT,
            t(6): 150, t(7): 0, t(8): 3, t(9): PRE_INT, t(10): 7, t(11): PRE_INT}
    got, b3 = stimulate(d, vals, [t(1), t(3), t(5), t(7), t(9), t(11)])
    if got is None:
        return
    check(got[t(1)] == 15, f"{label}: K-01 INT ADD 10 + const 5 = 15 (got {s32(got[t(1)])})")
    check(got[t(3)] == 0x41200000, f"{label}: K-02 REAL MUL 4.0 * const 2.5 = 10.0 (got 0x{got[t(3)]:08X}; "
          f"0 would mean Reg 14/15 words swapped)")
    check(got[t(5)] == 1, f"{label}: K-03 INT SUB 0 - const(-1) = 1 (got {s32(got[t(5)])})")
    check(got[t(7)] == 5, f"{label}: non-MATH SET rule (Reg 13 low 0x5A, Reg 14..15 junk) still fires (got {got[t(7)]})")
    check(got[t(9)] == 3 * 0x00010002,
          f"{label}: C-12 Reg 6..7 ignored, const 0x00010002: 3 * 65538 = 196614 (got {s32(got[t(9)])}; "
          f"131073 would mean the const words are swapped)")
    check(got[t(11)] == PRE_INT, f"{label}: disabled MATH rule does not write OUT (got {s32(got[t(11)])})")
    check(b3 == 0, f"{label}: no fault in this set -> health bit 3 = 0 (got {b3})")


def step_vectors(d, vr0, hold_s):
    X, Y, Z = vr0, vr0 + 1, vr0 + 2
    ok_all = True
    for kind, op, flags, preset, vecs in groups():
        title = f"{kind} {OPNAME[op]}"
        print(f"\n2. {title} (flags 0x{flags:02X}): one-rule COMMIT, Op1 = tag {X}, Op2 = tag {Y}, OUT = tag {Z}")
        # Safe operands first, so the previous group's rule (other operator/type) cannot keep faulting.
        diag_set(d, {X: ONE_F, Y: ONE_F, Z: preset})
        rules = [math_rule(X, Y, Z, op, flags)]
        if not deploy(d, rules, f"{title}"):
            ok_all = False
            continue
        check(read_active(d, 1) == flat(rules), f"{title}: rule reads back byte-exact")
        wait_fault_clear(d, hold_s, title)
        prev_fault = False
        for vid, a, b, out, fault, note in sorted(vecs, key=lambda v: v[4]):    # fault-free first
            if fault and prev_fault:
                # Bit 3 is held ~hold ms after the LAST fault: without this wait a vector that fails
                # to flag its own fault would be masked by the previous vector's fault.
                diag_set(d, {X: ONE_F, Y: ONE_F, Z: preset})
                wait_fault_clear(d, hold_s, f"before {vid}")
            prev_fault = bool(fault)
            got, b3 = stimulate(d, {X: u32v(a), Y: u32v(b), Z: preset}, [Z])
            if got is None:
                ok_all = False
                continue
            exp_out = preset if out is None else u32v(out)
            ok = got[Z] == exp_out and b3 == fault
            ok_all &= ok
            check(ok, f"{vid} {title}: {fmt(kind, a)} , {fmt(kind, b)} -> {fmt(kind, got[Z])}, bit3={b3}"
                  + (f"   (expected {fmt(kind, exp_out)}, bit3={fault})" if not ok else "")
                  + (f"   [{note}]" if note and ok else ""))
    return ok_all


def step_systag(d, vr0):
    X, Z, W = vr0, vr0 + 2, vr0 + 3
    print("\n3. System tags as operands (own addition): X + SYS_ON(1001) -> Z, X / SYS_OFF(1002) -> W")
    sysv = d.read(REG_SYSTAG, 3)
    if sysv[1] != 1 or sysv[2] != 0:
        info(f"System tags not served (0x0821 = {sysv[1]}, 0x0822 = {sysv[2]}): SPLC_SYSTAG_ENABLE=0? step skipped")
        return
    diag_set(d, {X: 1, Z: PRE_INT, W: PRE_INT})
    rules = [math_rule(X, SYS_ON, Z, OP_ADD, FLAGS_INT), math_rule(X, SYS_OFF, W, OP_DIV, FLAGS_INT)]
    if not deploy(d, rules, "system-tag set"):
        return
    got, b3 = stimulate(d, {X: 41, Z: PRE_INT, W: PRE_INT}, [Z, W])
    if got is None:
        return
    check(got[Z] == 42, f"41 + SYS_ON = 42 (got {s32(got[Z])})")
    check(got[W] == PRE_INT and b3 == 1,
          f"41 / SYS_OFF(0) -> OUT kept, bit 3 set (OUT {s32(got[W])}, bit3 {b3})")
    diag_set(d, {X: 1, Z: PRE_INT, W: PRE_INT})       # leave a quiet engine for the next step


def step_timing(d, vr0, hold_s):
    X, Y, Z = vr0, vr0 + 1, vr0 + 2
    print(f"\n4. Health bit 3 timing (hold = {hold_s:.1f} s)")
    diag_set(d, {X: ONE_F, Y: ONE_F, Z: PRE_INT})
    if not deploy(d, [math_rule(X, Y, Z, OP_DIV, FLAGS_INT)], "INT DIV set"):
        return
    wait_fault_clear(d, hold_s, "start")
    time.sleep(1.0)
    check(health_bit3(d) == 0, "no fault -> bit 3 stays 0 (1 s observed)")
    diag_set(d, {X: 5, Y: 0, Z: PRE_INT})
    time.sleep(SETTLE_S)
    check(health_bit3(d) == 1, "fault (5 / 0) -> bit 3 set")
    time.sleep(hold_s + 0.8)
    check(health_bit3(d) == 1, f"fault still active after {hold_s + 0.8:.1f} s -> bit 3 still set (refreshed, not a one-shot)")
    # Stop the fault: the last fault happened just before ENTER_DIAG takes effect.
    d.fc06(T.REG_DIAG, T.CMD_ENTER)
    t0 = time.time()
    if not TT.wait_diag_state(d, TT.ST_DIAG):
        check(False, "ENTER_DIAG reached DIAG_CONTROL")
        return
    d.fc16(T.REG_TAGS + 2 * Y, [0, 1])
    d.fc06(T.REG_DIAG, T.CMD_EXIT)
    t_clear, early = None, None
    lo, hi = hold_s - 0.5, hold_s + 0.8
    while time.time() - t0 < hi + 1.5:
        b = health_bit3(d)
        dt = time.time() - t0
        if b == 0:
            t_clear = dt
            break
        time.sleep(0.05)
    if t_clear is None:
        check(False, f"bit 3 fell by itself within {hi + 1.5:.1f} s after the last fault")
        return
    info(f"bit 3 fell {t_clear:.2f} s after ENTER_DIAG (last fault just before it)")
    check(lo <= t_clear <= hi, f"bit 3 falls about the hold time after the last fault ({lo:.1f} .. {hi:.1f} s window, got {t_clear:.2f} s)")
    time.sleep(1.0)
    check(health_bit3(d) == 0 and TT.tag_value(d, Z) == 5, "bit 3 stays 0 afterwards, rule runs again (5 / 1 = 5)")


def step_scale(d, vr0):
    print("\n5. SCALE_TAG flags (spec section 7, vectors S-01..S-03)")
    f = lambda x: bits_from_f32(x)
    cases = [   # (flags, src, gain, offset, expected dst, label)
        (0x07, 100, f(1.0), f(0.0), 0x42C80000, "S-01 0x07: INT 100 * 1 + 0 = 100.0f"),
        (0x07, u32v(-5), f(2.0), f(0.5), 0xC1180000, "S-02 0x07: INT -5 * 2 + 0.5 = -9.5f"),
        (0x07, 16777217, f(1.0), f(0.0), 0x4B800000, "S-03 0x07: INT 2^24+1 -> 16777216.0f (rounds to nearest)"),
        (0x03, 0x42C80000, f(1.0), f(0.0), 0x42C80000, "0x03 still reads the source as float32 bits (100.0f stays 100.0f)"),
        (0x05, 3, 2000, 5, 11, "0x05: bit 2 without both float bits -> INTEGER formula 3*2000/1000+5 = 11"),
        (0x06, 3, 2000, 5, 11, "0x06: bit 2 + bit 1 only -> INTEGER formula = 11"),
    ]
    rules = [scale_rule(vr0 + 2 * i, vr0 + 2 * i + 1, c[0], c[2], c[3]) for i, c in enumerate(cases)]
    vals = {}
    for i, c in enumerate(cases):
        vals[vr0 + 2 * i], vals[vr0 + 2 * i + 1] = c[1], PRE_INT
    diag_set(d, vals)
    if not deploy(d, rules, "SCALE_TAG set"):
        return
    act = read_active(d, len(rules))
    check([act[16 * i + 13] for i in range(len(rules))] == [c[0] << 8 for c in cases],
          "flags echoed verbatim: " + " ".join(f"0x{c[0]:02X}" for c in cases))
    got, b3 = stimulate(d, vals, [vr0 + 2 * i + 1 for i in range(len(cases))])
    if got is None:
        return
    for i, c in enumerate(cases):
        v = got[vr0 + 2 * i + 1]
        check(v == c[4], f"{c[5]}  (got 0x{v:08X}, expected 0x{c[4]:08X})")
    check(b3 == 0, "SCALE_TAG sets no MATH fault (bit 3 = 0)")


def step_reject(d, vr0):
    print("\n6. COMMIT rejections (spec 5 / 8.5): status 4, error 2, version and running table unchanged")
    X, Y, Z = vr0, vr0 + 1, vr0 + 2
    diag_set(d, {X: ONE_F, Y: ONE_F, Z: PRE_INT})
    base = [math_rule(X, Y, Z, OP_ADD, FLAGS_INT)]
    if not deploy(d, base, "base rule (INT ADD)"):
        return

    def rejected(name, rule):
        v0 = d.read(T.REG_VERSION, 1)[0]
        before = read_active(d, 1)
        st, er = commit_raw(d, [rule])
        check(st == ST_ERROR and er == ERR_INVALID, f"{name}: COMMIT refused (status {st}, error {er}; expect {ST_ERROR}/{ERR_INVALID})")
        check(d.read(T.REG_VERSION, 1)[0] == v0, f"{name}: active version did not move")
        check(read_active(d, 1) == before and T.rule_count(d) == 1, f"{name}: running table unchanged")

    def r(**kw):
        args = dict(op1=X, op2=Y, out=Z, op=OP_ADD, flags=FLAGS_INT)
        args.update(kw)
        return math_rule(**args)

    rejected("C-01 flags 0x01", r(flags=0x01))
    rejected("C-02 flags 0x02", r(flags=0x02))
    rejected("C-03 flags 0x04", r(flags=0x04))
    rejected("C-04 flags 0x07", r(flags=0x07))
    rejected("C-04b flags 0x80", r(flags=0x80))
    rejected("C-05 compare_op 5", r(compare_op=5))
    rejected("C-06 DIV INT const 0", r(op=OP_DIV, op2=None, const=0))
    rejected("C-07 DIV REAL const -0.0", r(op=OP_DIV, flags=FLAGS_REAL, op2=None, const=0x80000000))
    rejected("C-07b DIV REAL const +0.0", r(op=OP_DIV, flags=FLAGS_REAL, op2=None, const=0x00000000))
    rejected("C-08 MOD INT const 0", r(op=OP_MOD, op2=None, const=0))
    rejected("C-09 OUT = system tag 1000", r(out=1000))
    rejected("C-10 Op1 outside the tag table (500)", r(op1=500))

    # Accepted: a disabled rule is not checked, Reg 6..7 are ignored.
    for name, rule in (("C-11 disabled rule with compare_op 5 / flags 0x01", r(enabled=0, compare_op=5, flags=0x01)),
                       ("C-12 Reg 6..7 set", r(param=0xDEADBEEF))):
        v0 = d.read(T.REG_VERSION, 1)[0]
        s, e = T.commit(d, [rule])
        check(s == T.CFG_READY and e == 0, f"{name}: accepted (status {s}, error {e})")
        check(d.read(T.REG_VERSION, 1)[0] != v0, f"{name}: active version moved")
        check(read_active(d, 1) == [w for w in rule], f"{name}: stored byte-exact")


# ------------------------------------------------------------------ run
def run(d, args):
    hold_s = args.hold_ms / 1000.0
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 12:
        check(False, f"need >= 12 VREG tags (dense layout DI, DO, AI, VFLAG, VREG, ...), board has {vreg}")
        return
    vr0 = di + do + ai + vflag
    info(f"X = {vr0}, Y = {vr0 + 1}, Z = {vr0 + 2}, verbatim/SCALE steps use tags {vr0}..{vr0 + 11}")

    print("\n0. Capabilities and health register")
    caps, err = d.try_read(REG_CAPS, 2)
    if err is not None and getattr(err, "link", False):
        raise LinkError(f"probe 0x0A08: {err}")
    if err is not None:
        check(False, f"0x0A08 readable ({err})")
        return
    v = (caps[0] << 16) | caps[1]
    check(bool(v & CAP_FLOAT32), f"0x0A08 announces FLOAT32 (0x0800), caps = 0x{v:08X}")
    if not check(bool(v & CAP_MATH), f"0x0A08 bit 16 (0x00010000) announces MATH, caps = 0x{v:08X}"):
        print("  firmware without MATH (SPLC_MATH_ENABLE=0 or an older build): nothing else to test")
        return
    hf, herr = d.try_read(REG_HEALTH_FLAGS, 1)
    check(herr is None, f"0x0803 (health_flags) readable ({hf if herr is None else herr})")

    print("\n1. Clean start")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
    check(T.rule_count(d) == 0, "rule count = 0")
    check(TT.wait_diag_state(d, TT.ST_ENGINE, timeout=1.0), "engine running (no leftover DIAG session)")
    wait_fault_clear(d, hold_s, "start state")
    check(health_bit3(d) == 0, "health bit 3 = 0 at start")

    rules = step_verbatim(d, vr0, "1a. Verbatim storage")
    if rules is not None:
        verify_verbatim_active(d, rules, "after COMMIT")
        run_verbatim(d, vr0, "running")
        if args.reboot:
            print("\n1b. Persistence across REBOOT")
            nd = d.reboot(args)
            if nd is None:
                check(False, "board came back after REBOOT")
                return
            d = nd
            verify_verbatim_active(d, rules, "after REBOOT (restored from Flash)")
            run_verbatim(d, vr0, "after REBOOT")

    step_vectors(d, vr0, hold_s)
    step_systag(d, vr0)
    step_timing(d, vr0, hold_s)
    step_scale(d, vr0)
    step_reject(d, vr0)

    print("\n7. Cleanup (CLEAR_RULES)")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48, help="registers per FC16 (default 48)")
    p.add_argument("--hold-ms", type=int, default=3000, help="SPLC_MATH_FAULT_HOLD_MS of the firmware (default 3000)")
    p.add_argument("--reboot", action="store_true", help="also check persistence across REBOOT")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    p.add_argument("--check-vectors", action="store_true", help="offline: compare the vector table with the Python model, no board")
    R.add_args(p)
    args = p.parse_args()
    if args.check_vectors:
        ok = check_vectors()
        print("\nALL PASS" if ok and T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
        sys.exit(0 if ok and T._failures == 0 else 1)
    if not args.port:
        p.error("port is required (or use --check-vectors)")
    R.start(args)
    T.Dev.chunk = max(1, min(args.chunk, 120))

    client = T.make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    d = T.Dev(client, args.unit)
    link_error = None
    try:
        run(d, args)
    except LinkError as e:
        link_error = e
        print(f"\n  [LINK] {e}")
        T.cleanup_after_abort(d)
    except Exception:
        import traceback
        traceback.print_exc()
        print("\n  [ERROR] unexpected exception in the test script (not a firmware verdict)")
        T.cleanup_after_abort(d)
        T._failures += 1
    finally:
        try:
            d.c.close()
        except Exception:
            pass
    if T.Dev.retried or T.Dev.retried_w:
        print(f"\n(info) lost frames needing a retry: {T.Dev.retried} read(s), {T.Dev.retried_w} write(s)")
    if link_error is not None:
        print("\nINCONCLUSIVE: a request outcome was unknown (link error), NOT a firmware failure."
              f" {T._failures} check(s) failed before it.")
        sys.exit(3)
    print("\nALL PASS" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
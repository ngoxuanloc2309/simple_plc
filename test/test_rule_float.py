#!/usr/bin/env python3
"""
test_rule_float.py - Verify the float32 Rule Engine of SimplePLC (decision 45 in
docs/handoff.md, step R2 of REAL) over Modbus RTU.

Requires: pip install pymodbus pyserial
Firmware must be built with SPLC_REAL_ENABLE=1.
Standalone like test_tagtype.py, whose helpers it imports (same folder, which in
turn imports test_mqtt.py): Dev, commit(), run_syscmd(), the register constants.

WARNING: this test WRITES Flash (replaces the rule table) and runs CLEAR_RULES.
It enters DIAG briefly to write tag values. Run it on a dev board only.

What is checked (plc_rule.h SPLC_RULE_FLAG_TRIGGER_FLOAT 0x01 / ACTION_FLOAT 0x02,
handoff decision 55):
  * Rule record byte `flags` = HIGH byte of wire register 13. It must go through
    staging, the CRC (computed over the wire image), the Active Rule Table read
    (0x0100..) and Flash, byte-exact.
  * A rule with flags bit 0 (TRIGGER_FLOAT) compares float32 BIT PATTERNS: GT,
    BETWEEN, EQ with an ABSOLUTE tolerance of 1e-5. ACT_SCALE_TAG runs
    y = x*gain + offset in float32 only with BOTH bits (0x03), bit-exact vs a
    float32 model; flags 0x01 or 0x02 alone run the integer formula.
  * SET_TAG writes action_param verbatim (a float param lands as the same bits).
  * An integer rule (flags = 0) next to float rules is unchanged.
  * Bit 1 alone (0x02) keeps the compare INTEGER (mixed rule: int trigger -> SET
    REAL); bit 0 alone (0x01) makes the compare float; bits 2..7 are stored
    and echoed but interpreted by nobody.
  * A COMMIT whose CRC was computed as if flags were 0 is rejected.
  * (--reboot) the table, flags included, survives a REBOOT and still runs.

Layout used (dense layout, tags read from DeviceResourceInfo; needs >= 8 VREG):
  X = vr0 (float trigger)   A = vr0+1 (X > 25.5 -> 1)   B = vr0+2 (SCALE of X)
  E = vr0+3 (X in [-1.5, 1.5] -> 7)   F = vr0+4 (X == 30.0 +-1e-5 -> 3)
  H = vr0+5 (float param 2.5 on any X != 0)   Y = vr0+6 (int trigger)
  G = vr0+7 (Y > 100 integer -> 5)

Usage:
  python test_rule_float.py COM17 --unit 1 --baudrate 115200
  python test_rule_float.py COM17 --reboot [--port-after COM18]

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port,
            3 = link error (inconclusive).
"""
import argparse
import struct
import sys
import time

import test_mqtt as T
import test_tagtype as TT
from test_mqtt import check, info, LinkError

# Rule enums (plc_rule.h)
TRG_ON_CHANGE = 0
OP_NONE, OP_EQ, OP_NEQ, OP_GT, OP_LT, OP_GTE, OP_LTE, OP_BETWEEN = range(8)
ACT_SET, ACT_SCALE = 0, 7
FLAG_TRIG = 0x01      # SPLC_RULE_FLAG_TRIGGER_FLOAT
FLAG_ACT = 0x02       # SPLC_RULE_FLAG_ACTION_FLOAT
FLAG_BOTH = 0x03      # legacy alias SPLC_RULE_FLAG_FLOAT_DATA
GAIN, OFFSET = 0.5, 1.25
THR_GT, THR_LO, THR_HI, THR_EQ, PARAM_F = 25.5, -1.5, 1.5, 30.0, 2.5
SETTLE_S = 0.25          # engine scan is 10 ms; leave margin for RS485 + diag exit


# ------------------------------------------------------------------ float32 model
def f32(x):
    """Round a Python float to float32 (what the firmware stores)."""
    return struct.unpack(">f", struct.pack(">f", x))[0]


def bits(x):
    """float32 bit pattern as an unsigned 32-bit int."""
    return struct.unpack(">I", struct.pack(">f", x))[0]


def as_float(u):
    return struct.unpack(">f", struct.pack(">I", u & 0xFFFFFFFF))[0]


def model(x):
    """Expected outputs {A,B,E,F,H,G} for trigger value x (float32 semantics of the firmware);
    keys must match stimulate()'s return value exactly."""
    x = f32(x)
    prod = f32(x * f32(GAIN))              # product rounded to float32 first (no FMA)
    return {
        "A": 1 if x > f32(THR_GT) else 0,
        "B": bits(f32(prod + f32(OFFSET))),
        "E": 7 if f32(THR_LO) <= x <= f32(THR_HI) else 0,
        "F": 3 if abs(x - f32(THR_EQ)) <= 1e-5 else 0,
        "H": bits(f32(PARAM_F)),
        "G": 0,                              # integer rule: Y stays 0 in the float stimuli
    }


# ------------------------------------------------------------------ encoding
def encode_rule_f(trig_tag, act_tag, op, act, lo=0, hi=0, for_ms=0, param=0, flags=0,
                  trig_type=TRG_ON_CHANGE, guard=T.GUARD_NONE, enabled=1):
    """16-register wire image with the rule flags in the HIGH byte of register 13."""
    return [*T.u32(lo), *T.u32(hi), *T.u32(for_ms), *T.u32(param),
            trig_tag & 0xFFFF, act_tag & 0xFFFF, guard & 0xFFFF,
            (enabled << 8) | trig_type, (op << 8) | act,
            (flags & 0xFF) << 8, 0, 0]


def build_rules(tg):
    """The rule set described in the module docstring. `tg` = dict of tag indices."""
    X, A, B, E, F, H, Y, G = (tg[k] for k in "XABEFHYG")
    return [
        encode_rule_f(X, A, OP_GT, ACT_SET, lo=bits(THR_GT), param=1, flags=FLAG_TRIG),
        encode_rule_f(X, B, OP_NONE, ACT_SCALE, hi=bits(OFFSET), param=bits(GAIN), flags=FLAG_BOTH),
        encode_rule_f(X, E, OP_BETWEEN, ACT_SET, lo=bits(THR_LO), hi=bits(THR_HI), param=7, flags=FLAG_TRIG),
        encode_rule_f(X, F, OP_EQ, ACT_SET, lo=bits(THR_EQ), param=3, flags=FLAG_TRIG),
        encode_rule_f(X, H, OP_NONE, ACT_SET, param=bits(PARAM_F), flags=FLAG_ACT),
        encode_rule_f(Y, G, OP_GT, ACT_SET, lo=100, param=5, flags=0),       # integer rule
    ]


def rules_crc(rules):
    return T.crc16_modbus(b"".join(T.rule_bytes(r) for r in rules))


# ------------------------------------------------------------------ device access

def read_active_chunked(d, n):
    out = []
    for i in range(n):
        out += d.read(0x0100 + 16 * i, 16)
    return out


def diag_set_tags_quiet(d, values):
    """ENTER DIAG, write {tag: uint32}, EXIT. Prints only on failure."""
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


def stimulate(d, tg, x_bits, y_val=0):
    """Zero every output, set X (and Y), leave DIAG (runtime reset => ON_CHANGE sees
    prev=0), wait for the engine, return the output tags as {name: uint32}."""
    vals = {tg["A"]: 0, tg["B"]: 0, tg["E"]: 0, tg["F"]: 0, tg["H"]: 0, tg["G"]: 0,
            tg["X"]: x_bits & 0xFFFFFFFF, tg["Y"]: y_val & 0xFFFFFFFF}
    if not diag_set_tags_quiet(d, vals):
        return None
    time.sleep(SETTLE_S)
    return {k: TT.tag_value(d, tg[k]) for k in "ABEFHG"}


def deploy(d, rules, what):
    s, e = T.commit(d, rules)
    return check(s == T.CFG_READY and e == 0, f"{what}: COMMIT ok (status {s}, error {e})")


def deploy_wrong_crc(d, rules, crc):
    """Stage + COMMIT like T.commit() but with a caller-chosen CRC. Returns (status, error)."""
    d.fc06(T.REG_RULE_COUNT_STAGED, len(rules))
    for i, rule in enumerate(rules):
        d.fc16(T.REG_STAGING + i * 16, rule)
    d.fc06(T.REG_EXPECTED_CRC, crc)
    d.fc06(T.REG_COMMIT, T.COMMIT_MAGIC, safe=False)
    time.sleep(0.3)
    return tuple(d.read(T.REG_STATUS, 2))


# ------------------------------------------------------------------ checks
X_CASES = [25.4, 25.6, 30.0, 30.000005, 30.0001, -7.5, 1.0, -1.5, 1.5, 1.6, 100.0, 0.1]


def check_stimuli(d, tg, label):
    print(f"\n{label}: float stimuli vs float32 model")
    bad = 0
    for x in X_CASES:
        got = stimulate(d, tg, bits(x))
        if got is None:
            return False
        exp = model(x)
        ok = got == exp
        if not ok:
            bad += 1
        check(ok, f"X={x!r:>10}: A={got['A']} B={got['B']:08X} E={got['E']} F={got['F']} H={got['H']:08X}"
                  + ("" if ok else f"   expected {exp} (B as float {as_float(got['B'])!r} vs {as_float(exp['B'])!r})"))
    return bad == 0


def check_integer_rule(d, tg):
    print("\nInteger rule (flags = 0) next to the float rules")
    # X = 0 so no float rule fires; Y alone drives the integer rule.
    got = stimulate(d, tg, 0, y_val=150)
    check(got is not None and got["G"] == 5, f"Y=150 > 100 (integer compare) -> G = 5 (got {None if got is None else got['G']})")
    got = stimulate(d, tg, 0, y_val=50)
    check(got is not None and got["G"] == 0, f"Y=50 -> G stays 0 (got {None if got is None else got['G']})")
    # float bits fed to an INTEGER rule are just a big integer: 25.4f = 0x41CB3333 > 100
    got = stimulate(d, tg, 0, y_val=bits(25.4))
    check(got is not None and got["G"] == 5, "integer rule sees float bits as an int32 (0x41CB3333 > 100)")


def run(d, args):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 8:
        print("Need >= 8 VREG tags (dense layout DI, DO, AI, VFLAG, VREG, ...)")
        return
    vr0 = di + do + ai + vflag
    tg = {k: vr0 + i for i, k in enumerate("XABEFHYG")}
    info("tags: " + ", ".join(f"{k}={v}" for k, v in tg.items()))
    rules = build_rules(tg)
    n = len(rules)

    print("\n0. Probe")
    regs, err = d.try_read(TT.REG_TYPES, TT.TYPES_REGS)
    if err is not None and getattr(err, "link", False):
        raise LinkError(f"probe 0x0080: {err}")
    if err is not None:
        print(f"  0x0080 not readable ({err}): firmware built with SPLC_REAL_ENABLE=0?")
        check(False, "REAL build detected (0x0080 readable)")
        return
    check(True, "REAL build detected (0x0080 readable)")

    print("\n1. Clean start: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")

    print("\n2. Deploy the float rule set; the CRC covers the flags byte")
    if not deploy(d, rules, "Deploy"):
        return
    active = read_active_chunked(d, n)
    want = [w for rule in rules for w in rule]
    check(active == want, "Active Rule Table reads back byte-exact (reg 13 high byte = 0x01/0x03/0x01/0x01/0x02 on the float rules)"
          + ("" if active == want else f" {TT.diff(active, want)}"))
    want_flags = [0x0100, 0x0300, 0x0100, 0x0100, 0x0200, 0x0000]
    check([active[16 * i + 13] for i in range(n)] == want_flags,
          "flags (reg 13 high byte): 0x01 GT, 0x03 SCALE, 0x01 BETWEEN, 0x01 EQ, 0x02 SET, 0x00 integer rule")

    ok_float = check_stimuli(d, tg, "3. Running")
    check_integer_rule(d, tg)

    print("\n4. A COMMIT whose CRC ignores the flags byte is rejected")
    zeroed = [list(r) for r in rules]
    for r_ in zeroed:
        r_[13] = 0
    v0 = d.read(T.REG_VERSION, 1)[0]
    st, er = deploy_wrong_crc(d, rules, rules_crc(zeroed))
    check(st != T.CFG_READY, f"COMMIT with CRC-of-zeroed-flags rejected (status {st}, error {er})")
    check(d.read(T.REG_VERSION, 1)[0] == v0, "active version did not move")
    check(read_active_chunked(d, n) == want, "running table unchanged after the rejected COMMIT")

    print("\n5. Orthogonal flag bits (decision 55)")
    X, Y, G, B, A = (tg[k] for k in "XYGBA")
    NEG1, NEG2 = bits(-1.0), bits(-2.0)     # as int32: -1082130432 < -1073741824, as float: -1.0 > -2.0

    # 5a. flags 0x02 = action float only: the compare stays INTEGER, SET writes the float bits
    mixed = list(rules)
    mixed[5] = encode_rule_f(Y, G, OP_GT, ACT_SET, lo=NEG2, param=bits(2.5), flags=FLAG_ACT)
    if deploy(d, mixed, "5a Deploy flags=0x02 (int trigger, float action)"):
        act = read_active_chunked(d, n)
        check(act[16 * 5 + 13] == 0x0200, f"flags echoed verbatim (0x{act[16 * 5 + 13]:04X})")
        got = stimulate(d, tg, 0, y_val=NEG1)
        check(got is not None and got["G"] == 0,
              "flags=0x02 compares as INTEGER (bits of -1.0f are below bits of -2.0f as int32 -> no fire)")
        got = stimulate(d, tg, 0, y_val=bits(1.0))
        check(got is not None and got["G"] == bits(2.5),
              f"integer compare true -> SET writes the 2.5f bits (got {None if got is None else hex(got['G'])})")

    # 5b. flags 0x01 = trigger float only: the same stimulus now FIRES (float compare)
    mixed[5] = encode_rule_f(Y, G, OP_GT, ACT_SET, lo=NEG2, param=5, flags=FLAG_TRIG)
    if deploy(d, mixed, "5b Deploy flags=0x01 (float trigger, int action)"):
        got = stimulate(d, tg, 0, y_val=NEG1)
        check(got is not None and got["G"] == 5, "flags=0x01 compares as FLOAT (-1.0 > -2.0 -> G = 5)")

    # 5c. SCALE_TAG is float ONLY with 0x03; 0x00 / 0x01 / 0x02 run the integer formula
    #     B = x*param/1000 + hi with x = 3, param = 2000, hi = 5  ->  11
    for fl in (0x00, FLAG_TRIG, FLAG_ACT):
        sc = list(rules)
        sc[1] = encode_rule_f(X, B, OP_NONE, ACT_SCALE, hi=5, param=2000, flags=fl)
        if deploy(d, sc, f"5c Deploy SCALE with flags=0x{fl:02X}"):
            got = stimulate(d, tg, 3)
            check(got is not None and got["B"] == 11,
                  f"flags=0x{fl:02X}: SCALE runs the integer formula (3*2000/1000+5 = 11, got {None if got is None else got['B']})")

    # 5d. bits 2..7 are stored and echoed, interpreted by nobody
    odd = list(rules)
    odd[0] = encode_rule_f(X, A, OP_GT, ACT_SET, lo=bits(THR_GT), param=1, flags=0x07)    # TRIG + ACT + bit 2
    odd[5] = encode_rule_f(Y, G, OP_GT, ACT_SET, lo=100, param=5, flags=0x80)             # unknown bit only
    if deploy(d, odd, "5d Deploy flags=0x07 / 0x80"):
        act = read_active_chunked(d, n)
        check(act[13] == 0x0700 and act[16 * 5 + 13] == 0x8000,
              f"flags echoed verbatim (0x{act[13]:04X} / 0x{act[16 * 5 + 13]:04X})")
        got = stimulate(d, tg, bits(25.6))
        check(got is not None and got["A"] == 1, "flags=0x07 still compares as float (25.6 > 25.5 -> A = 1)")
        got = stimulate(d, tg, 0, y_val=150)
        check(got is not None and got["G"] == 5, "flags=0x80 compares as INTEGER (Y=150 > 100 -> G = 5)")
    deploy(d, rules, "Re-deploy the original set")

    if args.reboot:
        print("\n6. Persistence across REBOOT")
        nd = d.reboot(args)
        if nd is None:
            check(False, "board came back after REBOOT")
        else:
            d = nd
            check(read_active_chunked(d, n) == want, "Active Rule Table (flags included) restored from Flash byte-exact")
            check_stimuli(d, tg, "6b. Running after REBOOT")

    print("\n7. Cleanup (CLEAR_RULES)")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
    return ok_float


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48, help="registers per FC16 (default 48)")
    p.add_argument("--reboot", action="store_true", help="also check persistence across REBOOT")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    args = p.parse_args()
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
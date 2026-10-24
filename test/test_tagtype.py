#!/usr/bin/env python3
"""
test_tagtype.py - Verify the Tag DataType Registry 0x0080..0x00BF of SimplePLC
(decisions 40 and 41 in docs/handoff.md, step R1) over Modbus RTU.

Requires: pip install pymodbus pyserial
Firmware must be built with SPLC_REAL_ENABLE=1.

Standalone like test_mqtt.py, whose helpers it imports (same folder): Dev,
commit(), run_syscmd(), encode_rule(), the register constants and check().

WARNING: this test WRITES Flash (rule table + tag types), REPLACES the rule
table and runs CLEAR_RULES / FACTORY_RESET (which also wipe the MQTT config).
It enters DIAG briefly to write a few tag values. Run it on a dev board only.

Semantics checked (plc_tagtype.h):
  * FC16/FC06 into 0x0080..0x00BF go to a DRAFT; reads return the RUNNING table.
  * Wire layout: register 0x0080+k = (type of tag 2k) << 8 | (type of tag 2k+1).
    Types: 0 = not declared (INT32), 1 = BOOL, 2 = INT32, 3 = REAL.
  * COMMIT validates the WHOLE draft first: a type byte > 3, or REAL on a DI /
    DO / VFLAG / COUNTER tag, rejects the entire COMMIT (CONFIG_STATUS != READY,
    error code 2); the running table does not change and the draft is dropped.
  * A Deploy that does not write the table keeps the running one.
  * A tag that changes between an integer type and REAL has its value reset to 0
    (VREG / VREG_RETAIN); BOOL <-> INT32 keeps the value.
  * Only a request that runs past 0x00BF (or starts in the gap below 0x0080)
    answers exception 0x02; content errors are caught at COMMIT.
  * The table survives REBOOT; CLEAR_RULES and FACTORY_RESET clear it,
    CLEAR_RETAIN does not.

Checks, in order:
  0. Probe 0x0080 readable.   1. Clean start (CLEAR_RULES).
  2. Draft invisible before COMMIT, byte-exact after it.
  3. A Deploy without the table keeps it.   4. FC06 keeps the other registers.
  5. COMMIT rejections (all-or-nothing, draft dropped), REAL accepted on VREG /
     VREG_RETAIN / AI.   6. Value reset on integer <-> REAL, BOOL <-> INT32 keeps.
  7. Address limits.   8. (--reboot) persistence and the three clear commands.
  9. REAL on the first index past the board's runtime tags: either rejected (the
     slot exists in the tag array with kind NONE, MAX_TAGS > runtime tag count, e.g.
     Gateway) or ignored (index >= MAX_TAGS, e.g. Zigbee-IO). Both are correct.
  10. Cleanup (CLEAR_RULES).

Usage:
    python test_tagtype.py COM35 --unit 1 --baudrate 115200
    python test_tagtype.py COM35 --reboot [--port-after COM36]

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port,
3 = link error (inconclusive).
"""
import argparse
import test_report as R   # Excel evidence report (--report / --no-report)
import sys
import time

import test_mqtt as T
from test_mqtt import check, info, LinkError, exc_code

REG_TYPES = 0x0080
TYPES_REGS = 64
UNDECL, BOOL, INT32, REAL = 0, 1, 2, 3
DIAG_STATE = T.REG_DIAG + 1
ST_ENGINE, ST_DIAG = 1, 2


# ------------------------------------------------------------------ encoding
def regs_from(types):
    """64-register image of {tag: type}."""
    img = bytearray(2 * TYPES_REGS)
    for tag, t in types.items():
        img[tag] = t
    return [(img[2 * k] << 8) | img[2 * k + 1] for k in range(TYPES_REGS)]


def read_types(d):
    return d.read(REG_TYPES, TYPES_REGS)


def write_types(d, types):
    """FC16 of the whole table into the draft; returns the first error or None."""
    return d.write_regs(REG_TYPES, regs_from(types))


def commit_with(d, rule, types=None):
    """Deploy like the App: optional table write, then stage + COMMIT."""
    if types is not None:
        resp = write_types(d, types)
        if resp is not None:
            return None, None, resp
    s, e = T.commit(d, [rule])
    return s, e, None


def deploy_ok(d, rule, types, what):
    s, e, resp = commit_with(d, rule, types)
    return check(resp is None and s == T.CFG_READY and e == 0,
                 f"{what}: COMMIT ok (status {s}, error {e})")


def deploy_rejected(d, rule, types, what):
    s, e, resp = commit_with(d, rule, types)
    check(resp is None, f"{what}: draft write ACKed (content is checked at COMMIT)")
    return check(s != T.CFG_READY and e == T.ERR_INVALID_PARAMETER,
                 f"{what}: COMMIT rejected (status {s}, error {e}; expect error 2)")


# ------------------------------------------------------------------ diag + tags
def tag_value(d, tag):
    r = d.read(T.REG_TAGS + 2 * tag, 2)
    return (r[0] << 16) | r[1]


def wait_diag_state(d, want, timeout=2.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if d.read(DIAG_STATE, 1)[0] == want:
            return True
        time.sleep(0.05)
    return False


def diag_set_tags(d, values):
    """ENTER DIAG, write {tag: uint32}, EXIT. Returns True when every step worked."""
    d.fc06(T.REG_DIAG, T.CMD_ENTER)
    if not wait_diag_state(d, ST_DIAG):
        check(False, "ENTER_DIAG reached DIAG_CONTROL")
        return False
    ok = True
    for tag, v in values.items():
        resp = d.fc16(T.REG_TAGS + 2 * tag, [(v >> 16) & 0xFFFF, v & 0xFFFF])
        ok = check(not resp.isError(), f"diag write tag {tag} = 0x{v:08X}") and ok
    d.fc06(T.REG_DIAG, T.CMD_EXIT)
    ok = check(wait_diag_state(d, ST_ENGINE), "EXIT_DIAG back to ENGINE_RUNNING") and ok
    return ok


def diff(a, b):
    return [f"reg {i}: {x:04X} != {y:04X}" for i, (x, y) in enumerate(zip(a, b)) if x != y][:4]


# ------------------------------------------------------------------ the test
def run(d, args):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} "
          f"RETAIN={retain} COUNTER={counter}")
    if vreg < 4 or vflag < 1:
        print("Need >= 4 VREG and >= 1 VFLAG tags (dense layout DI, DO, AI, VFLAG, VREG, RETAIN, COUNTER)")
        return
    di0, do0 = 0, di
    ai0 = di + do
    vf0 = ai0 + ai
    vr0 = vf0 + vflag
    rt0 = vr0 + vreg
    ct0 = rt0 + retain
    total = ct0 + counter
    # vr0 is used by the rule only and never typed; vr0+1.. are the test tags.
    T_REAL, T_INT, T_VAL = vr0 + 1, vr0 + 2, vr0 + 3
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)

    # ------------------------------------------------------------------ 0
    print("\n0. Probe")
    regs, err = d.try_read(REG_TYPES, TYPES_REGS)
    if err is not None and getattr(err, "link", False):
        raise LinkError(f"probe 0x0080: {err}")
    if err is not None:
        print(f"  0x0080 not readable ({err}): firmware built with SPLC_REAL_ENABLE=0?")
        check(False, "tag-type area readable")
        return
    check(len(regs) == TYPES_REGS, "0x0080..0x00BF readable (64 registers)")

    # ------------------------------------------------------------------ 1
    print("\n1. Clean start: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
    check(all(v == 0 for v in read_types(d)), "all 64 registers read 0 (every tag undeclared)")
    exp = {}

    # ------------------------------------------------------------------ 2
    print("\n2. Draft invisible before COMMIT, byte-exact after it")
    want = {T_REAL: REAL, T_INT: INT32, vf0: BOOL}
    resp = write_types(d, want)
    check(resp is None, "FC16 of the whole table accepted")
    check(all(v == 0 for v in read_types(d)), "read before COMMIT still returns the RUNNING table (empty)")
    if deploy_ok(d, rule, None, "Deploy"):
        exp = dict(want)
    got = read_types(d)
    check(got == regs_from(exp), f"table reads back byte-exact {diff(got, regs_from(exp)) or ''}")
    k = T_REAL // 2
    shift = 8 if T_REAL % 2 == 0 else 0
    check(((got[k] >> shift) & 0xFF) == REAL,
          f"byte order: tag {T_REAL} is the {'High' if shift else 'Low'} byte of register 0x{REG_TYPES + k:04X}")

    # ------------------------------------------------------------------ 3
    print("\n3. A Deploy that does not write the table keeps it")
    deploy_ok(d, rule, None, "Deploy without the table")
    check(read_types(d) == regs_from(exp), "table unchanged")

    # ------------------------------------------------------------------ 4
    print("\n4. FC06 changes one register, the rest of the table is kept")
    t4 = vr0 + 4 if vreg >= 6 else None
    if t4 is None:
        info("fewer than 6 VREG: skipped")
    else:
        k4 = t4 // 2
        cur = read_types(d)[k4]
        new = (cur & 0xFF00) | REAL if t4 % 2 else (cur & 0x00FF) | (REAL << 8)
        resp = d.fc06(REG_TYPES + k4, new)
        check(not resp.isError(), "FC06 into the area accepted")
        check(read_types(d) == regs_from(exp), "before COMMIT the running table is unchanged")
        if deploy_ok(d, rule, None, "Deploy"):
            exp[t4] = REAL
        check(read_types(d) == regs_from(exp), "after COMMIT: the new tag AND the earlier ones are there")

    # ------------------------------------------------------------------ 5
    print("\n5. COMMIT rejections are all-or-nothing; REAL accepted on VREG / RETAIN / AI")
    bad_cases = [("REAL on DI0", di0, REAL, di > 0), ("REAL on DO0", do0, REAL, do > 0),
                 ("REAL on VFLAG0", vf0, REAL, True), ("REAL on COUNTER0", ct0, REAL, counter > 0),
                 ("type 4 on a VREG", T_INT, 4, True), ("type 0xFF on a VREG", T_INT, 0xFF, True)]
    for name, tag, typ, applicable in bad_cases:
        if not applicable:
            info(f"{name}: board has no such tag, skipped")
            continue
        types = dict(exp)
        types[tag] = typ
        types[vr0 + 5 if vreg > 5 else T_VAL] = REAL      # a GOOD change in the same draft
        deploy_rejected(d, rule, types, name)
        check(read_types(d) == regs_from(exp), f"{name}: running table unchanged (the good entry was not applied)")
    deploy_ok(d, rule, None, "Deploy after the rejections")
    check(read_types(d) == regs_from(exp), "draft was dropped by the rejected COMMITs (nothing leaks into the next one)")
    if retain > 0:
        types = dict(exp)
        types[rt0] = REAL
        if deploy_ok(d, rule, types, "REAL on VREG_RETAIN"):
            exp = types
    if ai > 0:
        types = dict(exp)
        types[ai0] = REAL
        if deploy_ok(d, rule, types, "REAL on AI"):
            exp = types
    check(read_types(d) == regs_from(exp), "table matches the accepted changes")

    # ------------------------------------------------------------------ 6
    print("\n6. Value reset on integer <-> REAL; BOOL <-> INT32 keeps the value")
    if diag_set_tags(d, {T_VAL: 123, vf0: 1}):
        check(tag_value(d, T_VAL) == 123 and tag_value(d, vf0) == 1,
              "tag values survive leaving DIAG (123 / 1)")
        types = dict(exp)
        types[T_VAL] = REAL
        if deploy_ok(d, rule, types, "VREG INT32 -> REAL"):
            exp = types
        check(tag_value(d, T_VAL) == 0, "...its value was reset to 0")
        if diag_set_tags(d, {T_VAL: 0x3F800000}):
            deploy_ok(d, rule, None, "Deploy without the table")
            check(tag_value(d, T_VAL) == 0x3F800000, "REAL -> REAL (no type change): value kept (1.0f)")
        types = dict(exp)
        types[T_VAL] = INT32
        if deploy_ok(d, rule, types, "VREG REAL -> INT32"):
            exp = types
        check(tag_value(d, T_VAL) == 0, "...its value was reset to 0")
        types = dict(exp)
        types[vf0] = INT32
        if deploy_ok(d, rule, types, "VFLAG BOOL -> INT32"):
            exp = types
        check(tag_value(d, vf0) == 1, "BOOL -> INT32: value kept")
    check(read_types(d) == regs_from(exp), "table matches the model")

    # ------------------------------------------------------------------ 7
    print("\n7. Address limits")
    last = read_types(d)[TYPES_REGS - 1]
    resp = d.fc16(REG_TYPES + TYPES_REGS - 1, [last, 0])
    check(resp.isError() and exc_code(resp) == T.EXC_ADDR, "FC16 0x00BF x2 (runs past the area) -> exception 0x02")
    resp = d.fc16(REG_TYPES - 1, [0, 0])
    check(resp.isError() and exc_code(resp) == T.EXC_ADDR, "FC16 0x007F x2 (starts in the gap) -> exception 0x02")
    resp = d.fc16(REG_TYPES + TYPES_REGS - 1, [last])
    check(not resp.isError(), "FC16 0x00BF x1 (last register, same value) accepted")
    regs, err = d.try_read(REG_TYPES + TYPES_REGS - 2, 4)
    check(err is None and regs[2:] == [0, 0], "FC03 across the end of the area: the two unmapped registers read 0")
    deploy_ok(d, rule, None, "Deploy (consumes the harmless draft)")
    check(read_types(d) == regs_from(exp), "table unchanged")

    # ------------------------------------------------------------------ 8
    if args.reboot:
        print("\n8. Persistence across REBOOT and the clear commands")
        nd = d.reboot(args)
        if nd is None:
            check(False, "board came back after REBOOT")
            return
        d = nd
        check(read_types(d) == regs_from(exp), "table identical after REBOOT (restored from Flash)")
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RETAIN)
        check(s == T.ST_DONE and e == 0, f"CLEAR_RETAIN result {s}/{e}")
        check(read_types(d) == regs_from(exp), "CLEAR_RETAIN keeps the table")
        for name, cmd in (("CLEAR_RULES", T.SYS_CLEAR_RULES), ("FACTORY_RESET", T.SYS_FACTORY)):
            deploy_ok(d, rule, exp, f"re-deploy the table before {name}")
            s, e = T.run_syscmd(d, cmd)
            check(s == T.ST_DONE and e == 0, f"{name} result {s}/{e}")
            check(all(v == 0 for v in read_types(d)), f"{name} clears the table")
            nd = d.reboot(args)
            if nd is None:
                check(False, "board came back after REBOOT")
                return
            d = nd
            check(all(v == 0 for v in read_types(d)), f"...and it stays clear after REBOOT ({name} was saved)")
        exp = {}
    else:
        print("\n8. (skipped: add --reboot for persistence and the clear commands)")

    # ------------------------------------------------------------------ 9
    print("\n9. REAL on the first index past the runtime tags (kind NONE or beyond MAX_TAGS)")
    if total < 128:
        types = {total: REAL, T_REAL: REAL}
        resp = write_types(d, types)
        check(resp is None, f"draft write with REAL on tag {total} ACKed")
        s, e = T.commit(d, [rule])
        if s == T.CFG_READY and e == 0:
            info(f"tag {total} is beyond MAX_TAGS on this board: the entry is ignored (accepted)")
            check(read_types(d) == regs_from(types), "stored as written (no tag to apply it to)")
        else:
            info(f"tag {total} exists with kind NONE (MAX_TAGS > {total}, e.g. Gateway): REAL refused")
            check(s != T.CFG_READY and e == T.ERR_INVALID_PARAMETER,
                  f"COMMIT rejected with error 2 (status {s}, error {e})")
            check(read_types(d) == regs_from(exp), "...and the running table is unchanged")
    else:
        info("the board has 128 tags: nothing beyond the table")

    # ------------------------------------------------------------------ 10
    print("\n10. Cleanup: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
    check(all(v == 0 for v in read_types(d)), "table empty again")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48,
                   help="registers per FC16 when writing the table (default 48)")
    p.add_argument("--reboot", action="store_true", help="also check persistence across REBOOT")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    R.add_args(p)
    args = p.parse_args()
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
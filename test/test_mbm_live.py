#!/usr/bin/env python3
"""
test_mbm_live.py - MB_READ live test against test/mbm_slave_sim.py: data types,
endianness, multiplier, FC01..04, and the error codes of the status tags.

Needs two serial ports, like the --live step of test_mpm.py:
    * the board's USB-CDC port (this script)      e.g. COM14
    * a USB-RS485 dongle on the board's RS485     e.g. COM35 (mbm_slave_sim.py)

WARNING: WRITES Flash (rule table + MB_READ blocks + tag types) and ends with
CLEAR_RULES. Dev board only. Firmware: SPLC_MBM_ENABLE=1, SPLC_REAL_ENABLE=1.

--expect none (default): the sim runs WITHOUT a fault. Deploys block sets and
    compares every target tag with a value computed here from the sim's register
    table (word order, float bits, multiplier, rounding):
      1. FC03 / FC04 with UINT16, INT16, UINT32, INT32, REAL, BOOL, the four
         endiannesses, multiplier 0.5 / 2.0 / 0.001 (target REAL and non-REAL)
      2. FC01 and FC02 (BOOL pins, one register per coil)
      3. the poll is continuous (the sim's counter register keeps growing)
      4. status tags: error = 0, err_code = 0 on every block
--expect timeout|crc|exception|short|late: restart the sim with the matching fault
    and run again. One block polls slave 1; the status tags must report
        timeout   -> error 1, err_code 1      sim: --fault timeout
        crc       -> error 1, err_code 2      sim: --fault crc
        exception -> error 1, err_code 3      sim: --fault exception
        short     -> error 1, err_code 4 or 1 sim: --fault short
        late      -> error 1, err_code 1      sim: --delay-ms 500 (answer after the
                                                  300 ms timeout; it must be dropped;
                                                  the block interval is 1500 ms here)
    and the target tag must NOT be written by a failed poll.

Usage (sim in another terminal first, same --baud as the firmware, 9600):
    python mbm_slave_sim.py COM35 --unit 1 --baud 9600
    python test_mbm_live.py COM14
    python mbm_slave_sim.py COM35 --fault crc
    python test_mbm_live.py COM14 --expect crc

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""
import argparse
import struct
import sys
import time

import test_mqtt as T
import test_tagtype as TT
import test_mpm as M
from test_mqtt import check, info, LinkError

SLAVE = 1

# ---- the sim's register table (mbm_slave_sim.py), kept in step with it by hand
SIM = [0] * 10
SIM[0], SIM[1] = 1234, 5678
SIM[3] = 0xFFFE
SIM[4], SIM[5] = struct.unpack(">HH", struct.pack(">f", 23.5))
SIM[6], SIM[7] = struct.unpack(">HH", struct.pack(">i", -12345))
SIM[8], SIM[9] = struct.unpack(">HH", struct.pack(">I", 100000))
# SIM[2] is a counter (+1 per answered request)

EN_ABCD, EN_CDAB, EN_BADC, EN_DCBA = 0, 1, 2, 3


# ------------------------------------------------------------ expected values
def w32(reg_hi, reg_lo, en):
    """Value of two registers for one endianness (A B = reg_hi, C D = reg_lo)."""
    a, b, c, d = reg_hi >> 8, reg_hi & 255, reg_lo >> 8, reg_lo & 255
    order = {EN_ABCD: (a, b, c, d), EN_CDAB: (c, d, a, b),
             EN_BADC: (b, a, d, c), EN_DCBA: (d, c, b, a)}[en]
    return int.from_bytes(bytes(order), "big")


def w16(reg, en):
    return ((reg & 0xFF) << 8 | reg >> 8) if en in (EN_BADC, EN_DCBA) else reg


def s32(u):
    u &= 0xFFFFFFFF
    return u - (1 << 32) if u & 0x80000000 else u


def s16(u):
    return u - 0x10000 if u & 0x8000 else u


def fbits(f):
    return struct.unpack(">I", struct.pack(">f", f))[0]


def bits_f(u):
    return struct.unpack(">f", struct.pack(">I", u & 0xFFFFFFFF))[0]


def rnd(x):
    """Round to nearest, halves away from zero (what plc_mbm's f2i() does)."""
    return int(x + 0.5) if x >= 0 else int(x - 0.5)


def tag_s32(d, t):
    return s32(TT.tag_value(d, t))


# ------------------------------------------------------------ the test
def setup(d):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain = r[3], r[4], r[5], r[6], r[7], r[8]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} RETAIN={retain}")
    if vreg < 32:
        print("Need >= 32 VREG tags (target tags vr0+1.., status tags vr0+20..vr0+31)")
        return None
    vr0 = di + do + ai + vflag
    caps, cerr = d.try_read(M.REG_CAPS, 2)
    if cerr is None:
        v = (caps[0] << 16) | caps[1]
        if not check(v & M.CAP_MODBUS_MASTER != 0, f"0x0A08 announces MODBUS_MASTER (caps 0x{v:08X})"):
            return None
    return vr0


def dep(d, rule, blocks, real_tags, what):
    """Deploy like the App: tag types (REAL for real_tags), the blocks (the others disabled), COMMIT."""
    resp = TT.write_types(d, {t: TT.REAL for t in real_tags})
    if resp is not None:
        check(False, f"{what}: tag type table write failed ({resp})")
        return False
    full = {i: blocks.get(i, M.block(enabled=False)) for i in range(M.BLOCKS)}
    return M.deploy_ok(d, rule_holder[0], full, what)


rule_holder = [None]


def run_none(d, vr0, timeout_ms):
    V = lambda i: vr0 + 1 + i              # value tag i   (i = 0..15)
    S = lambda blk: (vr0 + 20 + 3 * blk, vr0 + 21 + 3 * blk, vr0 + 22 + 3 * blk)   # valid, err, errcode

    def blk(i, fc, count, pins, interval=200, addr=0):
        v, e, c = S(i)
        return M.block(fc=fc, slave=SLAVE, start=addr, count=count, interval=interval,
                       timeout=timeout_ms, valid=v, err=e, errcode=c, pins=pins)

    P = M.pin
    DT = M

    # ------------------------------------------------------------- 1
    print("\n1. FC03 / FC04, every type, endianness and multiplier")
    exp = {}
    b0 = blk(0, 3, 10, [P(V(0), 0, DT.DT_UINT16), P(V(1), 3, DT.DT_INT16),
                        P(V(2), 4, DT.DT_REAL), P(V(3), 6, DT.DT_INT32)])
    exp[V(0)], exp[V(1)] = SIM[0], s16(SIM[3])
    exp[V(2)], exp[V(3)] = fbits(23.5), -12345
    b1 = blk(1, 4, 10, [P(V(4), 8, DT.DT_UINT32), P(V(5), 6, DT.DT_INT32, EN_CDAB),
                        P(V(6), 4, DT.DT_REAL, EN_ABCD, 2.0), P(V(7), 2, DT.DT_UINT16)])
    exp[V(4)] = s32(w32(SIM[8], SIM[9], EN_ABCD))
    exp[V(5)] = s32(w32(SIM[6], SIM[7], EN_CDAB))
    exp[V(6)] = fbits(47.0)
    b2 = blk(2, 3, 10, [P(V(8), 0, DT.DT_INT16, EN_ABCD, 0.5), P(V(9), 0, DT.DT_UINT16, EN_ABCD, 0.5),
                        P(V(10), 6, DT.DT_INT32, EN_ABCD, 0.001), P(V(11), 0, DT.DT_BOOL)])
    exp[V(8)] = rnd(s16(SIM[0]) * 0.5)          # target not REAL: rounded integer (617)
    exp[V(9)] = fbits(SIM[0] * 0.5)             # target REAL: float bits (617.0)
    exp[V(10)] = rnd(-12345 * 0.001)            # -12.345 -> -12
    exp[V(11)] = 1
    b3 = blk(3, 3, 10, [P(V(12), 1, DT.DT_UINT16), P(V(13), 3, DT.DT_INT16, EN_BADC),
                        P(V(14), 0, DT.DT_UINT16, EN_DCBA), P(V(15), 8, DT.DT_UINT32, EN_DCBA)])
    exp[V(12)] = SIM[1]
    exp[V(13)] = s16(w16(SIM[3], EN_BADC))      # 0xFFFE byte-swapped = 0xFEFF = -257
    exp[V(14)] = w16(SIM[0], EN_DCBA)           # 0x04D2 -> 0xD204 = 53764
    exp[V(15)] = s32(w32(SIM[8], SIM[9], EN_DCBA))   # UINT32 above INT32_MAX wraps into the int32 tag
    real_tags = {V(2), V(6), V(9)}              # tags that hold float bits
    if not dep(d, rule_holder[0], {0: b0, 1: b1, 2: b2, 3: b3}, real_tags, "Deploy 4 blocks"):
        return
    time.sleep(2.0)
    names = {V(0): "UINT16 off0", V(1): "INT16 off3 (-2)", V(2): "REAL 23.5",
             V(3): "INT32 -12345", V(4): "UINT32 100000", V(5): "INT32 CDAB",
             V(6): "REAL 23.5 x 2.0", V(8): "INT16 x 0.5 -> int", V(9): "UINT16 x 0.5 -> REAL tag",
             V(10): "INT32 x 0.001", V(11): "BOOL", V(12): "UINT16 off1", V(13): "INT16 BADC",
             V(14): "UINT16 DCBA", V(15): "UINT32 DCBA (wraps)"}
    for t in sorted(exp):
        got = tag_s32(d, t)
        want = exp[t]
        extra = f" = {bits_f(got):g}" if t in real_tags else ""
        check(got == want, f"{names[t]}: tag {t} = {got} (0x{got & 0xFFFFFFFF:08X}){extra}, expect {want} (0x{want & 0xFFFFFFFF:08X})")

    # ------------------------------------------------------------- 2
    print("\n2. The poll is continuous (sim counter, register 2)")
    c1 = TT.tag_value(d, V(7))
    time.sleep(1.0)
    c2 = TT.tag_value(d, V(7))
    check(c2 - c1 >= 3, f"counter {c1} -> {c2} in 1 s (interval 200 ms, 4 blocks share the bus): grew by {c2 - c1}")

    # ------------------------------------------------------------- 3
    print("\n3. Status tags on all four blocks")
    for i in range(4):
        v, e, c = S(i)
        ev, cv = TT.tag_value(d, e), TT.tag_value(d, c)
        check(ev == 0 and cv == 0, f"block {i}: error={ev} err_code={cv} (expect 0/0)")

    # ------------------------------------------------------------- 4
    for fc, name in ((1, "FC01 coils"), (2, "FC02 discrete inputs")):
        print(f"\n4. {name}: coil n is set when n is even (sim)")
        b = blk(0, fc, 8, [P(V(0), 0, DT.DT_BOOL), P(V(1), 1, DT.DT_BOOL),
                           P(V(2), 2, DT.DT_BOOL), P(V(3), 3, DT.DT_BOOL)])
        if not dep(d, rule_holder[0], {0: b}, set(), f"Deploy {name}"):
            continue
        time.sleep(1.2)
        got = [TT.tag_value(d, V(i)) for i in range(4)]
        check(got == [1, 0, 1, 0], f"{name}: pins read {got}, expect [1, 0, 1, 0]")
        v, e, c = S(0)
        check(TT.tag_value(d, e) == 0 and TT.tag_value(d, c) == 0, f"{name}: error=0 err_code=0")

    deploy_off(d)


def deploy_off(d):
    dep(d, rule_holder[0], {}, set(), "Disable all blocks")


def run_fault(d, vr0, expect, timeout_ms):
    T0 = vr0 + 1
    v, e, c = vr0 + 20, vr0 + 21, vr0 + 22
    codes = {"timeout": (1,), "crc": (2,), "exception": (3,), "short": (4, 1), "late": (1,)}[expect]
    print(f"\nFault '{expect}': block 0 polls slave {SLAVE}; expect error=1, err_code in {codes}")
    before = TT.tag_value(d, T0)
    # late: the answer comes 500 ms after the 300 ms timeout. With a short interval the next
    # request would already be on the bus and that stale answer would match it (Modbus has no
    # transaction id), so the interval is longer than the delay: the late bytes must be thrown
    # away before the next request.
    interval = 1500 if expect == "late" else 200
    b = M.block(fc=3, slave=SLAVE, start=0, count=2, interval=interval, timeout=timeout_ms,
                valid=v, err=e, errcode=c, pins=[M.pin(T0, 0, M.DT_UINT16)])
    if not dep(d, rule_holder[0], {0: b}, set(), "Deploy block 0"):
        return
    seen_valid, last = False, None
    t_end = time.time() + (5.0 if expect == "late" else 3.0)
    while time.time() < t_end:
        vals = [TT.tag_value(d, t) for t in (v, e, c, T0)]
        seen_valid = seen_valid or vals[0] == 1
        last = vals
        time.sleep(0.05)
    ok_v, err_v, code_v, val = last
    info(f"last status: valid={ok_v} error={err_v} err_code={code_v} value={val}; valid pulse seen: {seen_valid}")
    check(err_v == 1 and code_v in codes, f"error={err_v}, err_code={code_v} (expect 1 / {codes})")
    check(not seen_valid, "valid never pulsed on a failed poll")
    check(val == before, f"a failed poll does not write the target tag ({before} -> {val})")
    deploy_off(d)


def run(d, args):
    vr0 = setup(d)
    if vr0 is None:
        return
    rule_holder[0] = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)
    print(f"\nSim expected on the bus: slave {SLAVE}, fault '{args.expect if args.expect != 'none' else 'none'}'")
    try:
        if args.expect == "none":
            run_none(d, vr0, args.timeout)
        else:
            run_fault(d, vr0, args.expect, args.timeout)
    finally:
        print("\nCleanup (CLEAR_RULES)")
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
        check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="board USB-CDC port, e.g. COM14")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48)
    p.add_argument("--expect", choices=["none", "timeout", "crc", "exception", "short", "late"], default="none",
                   help="which fault the sim was started with (default none)")
    p.add_argument("--timeout", type=int, default=300, help="block timeout in ms (default 300)")
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
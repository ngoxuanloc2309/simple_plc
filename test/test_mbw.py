#!/usr/bin/env python3
"""
test_mbw.py - Verify the MB_WRITE blocks 0x1100..0x11FF of SimplePLC (decision 48 in
docs/handoff.md, plc_mbw) over Modbus RTU.

Requires: pip install pymodbus pyserial
Firmware must be built with SPLC_MBM_ENABLE=1 and SPLC_MBW_ENABLE=1.

Standalone like test_mpm.py (test_mbm.py), whose helpers it imports from the same folder.

WARNING: this test WRITES Flash (rule table + MB_WRITE blocks), REPLACES the rule table and
runs CLEAR_RULES / FACTORY_RESET. Run it on a dev board only.

What it checks (no slave needed): the CONFIGURATION path (plc_mbw.h):
  * FC16/FC06 into 0x1100..0x11FF go to a DRAFT; reads return the RUNNING blocks.
  * COMMIT validates every drafted block first. A bad enabled block (port beyond the
    device, slave outside 1..247, FC outside {5,6,15,16}, FC05/06 with count != 1, count
    outside 1..16, a coil FC with a non-BOOL pin, FC06 with a 32-bit pin, overlapping pins,
    a source tag that does not exist, no pin in Direct mode, write mode > 1, channel count
    outside 1..32, timeout < 10 ms, cyclic interval < 10 ms, trigger mode > 2, an edge
    trigger tag beyond the tags, a status tag that is DI/DO/AI/COUNTER) rejects the WHOLE
    COMMIT (CONFIG_STATUS != READY, error 2); the running blocks do not change.
  * A block with the enable bit clear is not checked.
  * A Deploy that does not write the blocks keeps them running.
  * FC06 changes one register; the rest of the block is kept.
  * Only a request that runs past 0x11FF answers exception 0x02.
  * The blocks survive REBOOT; CLEAR_RULES and FACTORY_RESET clear them, CLEAR_RETAIN
    does not.
What it does NOT check: the frames on the RS-485 bus (needs a slave simulator that
understands FC05/06/15/16; test/mbm_slave_sim.py only answers reads).

Usage:
    python test_mbw.py COM35 --unit 1 --baudrate 115200 [--reboot] [--port-after COM36]

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""
import argparse
import test_report as R   # Excel evidence report (--report / --no-report)
import struct
import sys

import test_mqtt as T
import test_tagtype as TT
from test_mqtt import check, info, LinkError

REG_MBW = 0x1100
BLOCKS, BLOCK_REGS = 4, 64
MBW_REGS = BLOCKS * BLOCK_REGS
NONE = 0xFFFF
DT_BOOL, DT_INT16, DT_UINT16, DT_INT32, DT_UINT32, DT_REAL = 1, 2, 3, 4, 5, 6
READ_CHUNK = 64


def f32_regs(f):
    u = struct.unpack(">I", struct.pack(">f", f))[0]
    return [u >> 16, u & 0xFFFF]


def pin(tag=NONE, off=0, dt=DT_INT16, endian=0, mult=1.0):
    return [tag, off, dt, endian, *f32_regs(mult), 0, 0, 0, 0, 0, 0]


def block(enabled=True, fc=16, slave=1, start=0, count=2, interval=500, timeout=300,
          trig_mode=2, trig_tag=NONE, done=NONE, busy=NONE, err=NONE, errcode=NONE,
          exclusive=False, mode=0, port=0, param_base=0, pins=(), channel=None):
    """64-register image of one MB_WRITE block (layout of plc_mbw.h)."""
    r = [0] * BLOCK_REGS
    r[0] = (1 if enabled else 0) | (2 if exclusive else 0) | (mode << 2) | (port << 4)
    r[1] = (fc << 8) | slave
    r[2], r[3] = start, count
    r[4], r[5] = param_base >> 16, param_base & 0xFFFF
    r[6], r[7] = interval >> 16, interval & 0xFFFF
    r[8], r[9] = timeout >> 16, timeout & 0xFFFF
    r[10], r[11] = trig_mode, trig_tag
    r[12], r[13], r[14], r[15] = done, busy, err, errcode
    if mode == 1:
        ch, cmd, par = channel if channel else (NONE, NONE, NONE)
        r[16], r[17], r[18] = ch, cmd, par
    else:
        ps = list(pins) + [pin()] * (4 - len(pins))
        for i, p in enumerate(ps):
            r[16 + 12 * i:16 + 12 * (i + 1)] = p
    return r


def read_mbw(d):
    out = []
    for off in range(0, MBW_REGS, READ_CHUNK):
        out += d.read(REG_MBW + off, READ_CHUNK)
    return out


def image(blocks):
    img = [0] * MBW_REGS
    for i, b in blocks.items():
        img[i * BLOCK_REGS:(i + 1) * BLOCK_REGS] = b
    return img


def write_blocks(d, blocks):
    for i, b in blocks.items():
        resp = d.write_regs(REG_MBW + i * BLOCK_REGS, b)
        if resp is not None:
            return resp
    return None


def deploy(d, rule, blocks):
    if blocks:
        resp = write_blocks(d, blocks)
        if resp is not None:
            return None, None, resp
    s, e = T.commit(d, [rule])
    return s, e, None


def deploy_ok(d, rule, blocks, what):
    s, e, resp = deploy(d, rule, blocks)
    return check(resp is None and s == T.CFG_READY and e == 0,
                 f"{what}: COMMIT ok (status {s}, error {e})")


def deploy_rejected(d, rule, blocks, what):
    s, e, resp = deploy(d, rule, blocks)
    check(resp is None, f"{what}: draft write ACKed (content is checked at COMMIT)")
    return check(s != T.CFG_READY and e == T.ERR_INVALID_PARAMETER,
                 f"{what}: COMMIT rejected (status {s}, error {e}; expect error 2)")


def run(d, args):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} RETAIN={retain} COUNTER={counter}")
    if vreg < 8 or vflag < 1:
        print("Need >= 8 VREG and >= 1 VFLAG tags")
        return
    di0, do0 = 0, di
    ai0 = di + do
    vf0 = ai0 + ai
    vr0 = vf0 + vflag
    ct0 = vr0 + vreg + retain
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)
    SRC = vr0 + 1            # source tags (plain VREGs)
    ST = vr0 + 2             # status tags: ST..ST+3

    print("\n0. Probe")
    resp = d.write_regs(REG_MBW, [0])
    if resp is not None:
        check(False, "0x1100 accepts a write: firmware built with SPLC_MBW_ENABLE=1 (needs SPLC_MBM_ENABLE=1)")
        print("  Build with SPLC_MBW_ENABLE 1 in splcopts.h, clean rebuild, flash. Stopping.")
        return
    check(True, "0x1100 accepts a write (MB_WRITE present)")
    deploy_ok(d, rule, None, "Deploy (drops the probe draft)")

    print("\n1. Clean start: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
    check(all(v == 0 for v in read_mbw(d)), "all 256 registers read 0 (every block disabled)")

    print("\n2. Draft invisible before COMMIT, byte-exact after it")
    b0 = block(fc=16, slave=3, start=0x50, count=4, interval=1000, timeout=300, trig_mode=0, trig_tag=vf0,
               done=ST, busy=ST + 1, err=ST + 2, errcode=ST + 3,
               pins=[pin(SRC, 0, DT_INT16), pin(SRC + 1, 1, DT_REAL, endian=1, mult=0.5),
                     pin(vf0, 3, DT_BOOL)])
    b1 = block(fc=15, slave=9, start=0x20, count=8, trig_mode=1, interval=200, port=0,
               pins=[pin(vf0, 0, DT_BOOL), pin(SRC, 5, DT_BOOL)])
    b2 = block(mode=1, exclusive=True, slave=1, start=0x100, count=18, param_base=0x120, trig_mode=0, trig_tag=vf0,
               channel=(SRC, SRC + 1, SRC + 2), done=ST)
    want = {0: b0, 1: b1, 2: b2}
    check(write_blocks(d, want) is None, "FC16 of three blocks accepted")
    check(all(v == 0 for v in read_mbw(d)), "read before COMMIT still returns the RUNNING blocks (empty)")
    exp = {}
    if deploy_ok(d, rule, None, "Deploy"):
        exp = dict(want)
    got = read_mbw(d)
    check(got == image(exp), f"blocks read back byte-exact {TT.diff(got, image(exp)) or ''}")

    print("\n3. A Deploy that does not write the blocks keeps them")
    deploy_ok(d, rule, None, "Deploy without the blocks")
    check(read_mbw(d) == image(exp), "blocks unchanged")

    print("\n4. FC06 changes one register, the rest of the block is kept")
    resp = d.fc06(REG_MBW + 7, 2500)         # block 0, interval low word
    check(not resp.isError(), "FC06 into the area accepted")
    check(read_mbw(d) == image(exp), "before COMMIT the running blocks are unchanged")
    if deploy_ok(d, rule, None, "Deploy"):
        exp[0] = list(exp[0])
        exp[0][7] = 2500
    check(read_mbw(d) == image(exp), "after COMMIT: the new interval AND the rest of the block")

    print("\n5. COMMIT rejections are all-or-nothing, the draft is dropped")

    def bad(name, applicable=True, **kw):
        if not applicable:
            info(f"{name}: board has no such tag, skipped")
            return
        pins = kw.pop("pins", [pin(SRC, 0, DT_INT16)])
        kw.setdefault("count", 2)
        base = block(pins=pins, **kw) if kw.get("mode", 0) == 0 else block(**kw)
        good = block(slave=2, count=2, pins=[pin(SRC, 0, DT_INT16)])
        deploy_rejected(d, rule, {3: good, 2: base}, name)
        check(read_mbw(d) == image(exp), f"{name}: running blocks unchanged (the good block was not applied)")

    bad("port 5", port=4)
    bad("slave 0", slave=0)
    bad("slave 248", slave=248)
    bad("function code 3", fc=3)
    bad("function code 7", fc=7)
    bad("FC06 with 2 points", fc=6, count=2)
    bad("FC05 with 2 points", fc=5, count=2, pins=[pin(vf0, 0, DT_BOOL)])
    bad("count 0", count=0)
    bad("count 17", count=17)
    bad("coil FC with an INT16 pin", fc=15, pins=[pin(SRC, 0, DT_INT16)])
    bad("FC06 with a REAL pin", fc=6, count=1, pins=[pin(SRC, 0, DT_REAL)])
    bad("overlapping pins", count=2, pins=[pin(SRC, 0, DT_INT32), pin(SRC + 1, 1, DT_INT16)])
    bad("pin offset outside the count", pins=[pin(SRC, 2, DT_INT16)])
    bad("data type 7", pins=[pin(SRC, 0, 7)])
    bad("endianness 4", pins=[pin(SRC, 0, DT_INT16, 4)])
    bad("source tag beyond the tags", pins=[pin(0x7FFF, 0, DT_INT16)])
    bad("Direct without a pin", pins=[])
    bad("timeout 5 ms", timeout=5)
    bad("cyclic interval 5 ms", trig_mode=2, interval=5)
    bad("trigger mode 3", trig_mode=3)
    bad("edge trigger tag beyond the tags", trig_mode=0, trig_tag=0x7FFF)
    bad("status tag = DI", di > 0, done=di0)
    bad("status tag = COUNTER", counter > 0, busy=ct0)
    bad("channel count 0", mode=1, count=0, channel=(SRC, SRC + 1, SRC + 2))
    bad("channel count 33", mode=1, count=33, channel=(SRC, SRC + 1, SRC + 2))
    bad("channel tag beyond the tags", mode=1, count=4, channel=(0x7FFF, NONE, NONE))
    bw = block(count=2, pins=[pin(SRC, 0)])
    bw[0] |= (2 << 2)                       # write mode 2
    deploy_rejected(d, rule, {2: bw}, "write mode 2")
    check(read_mbw(d) == image(exp), "write mode 2: running blocks unchanged")
    deploy_ok(d, rule, None, "Deploy after the rejections")
    check(read_mbw(d) == image(exp), "draft was dropped by the rejected COMMITs")

    junk = [0xABAB] * BLOCK_REGS
    junk[0] = 0
    if deploy_ok(d, rule, {2: junk}, "disabled block with garbage content"):
        exp[2] = junk
    check(read_mbw(d) == image(exp), "the disabled block is stored as written (block 2 is off)")

    print("\n6. Address limits")
    last = read_mbw(d)[MBW_REGS - 1]
    resp = d.fc16(REG_MBW + MBW_REGS - 1, [last, 0])
    check(resp.isError() and T.exc_code(resp) == 0x02, "FC16 running past 0x11FF -> exception 0x02")
    check(read_mbw(d) == image(exp), "...and nothing changed")
    check(not d.fc16(REG_MBW + MBW_REGS - 1, [last]).isError(), "FC16 of the very last register accepted")
    deploy_ok(d, rule, None, "Deploy (drops the draft)")
    check(read_mbw(d) == image(exp), "blocks unchanged after the address tests")

    if args.reboot:
        print("\n7. Persistence across REBOOT and the clear commands")
        nd = d.reboot(args)
        if nd is None:
            check(False, "board came back after REBOOT")
            return
        d = nd
        check(read_mbw(d) == image(exp), "blocks identical after REBOOT (restored from Flash)")
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RETAIN)
        check(s == T.ST_DONE and e == 0, f"CLEAR_RETAIN result {s}/{e}")
        check(read_mbw(d) == image(exp), "CLEAR_RETAIN keeps the blocks")
        for name, cmd in (("CLEAR_RULES", T.SYS_CLEAR_RULES), ("FACTORY_RESET", T.SYS_FACTORY)):
            deploy_ok(d, rule, exp, f"re-deploy the blocks before {name}")
            s, e = T.run_syscmd(d, cmd)
            check(s == T.ST_DONE and e == 0, f"{name} result {s}/{e}")
            check(all(v == 0 for v in read_mbw(d)), f"{name} clears the blocks")
            nd = d.reboot(args)
            if nd is None:
                check(False, "board came back after REBOOT")
                return
            d = nd
            check(all(v == 0 for v in read_mbw(d)), f"...and they stay clear after REBOOT ({name} was saved)")
    else:
        print("\n7. (skipped: add --reboot for persistence and the clear commands)")

    print("\n8. Cleanup (CLEAR_RULES)")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
    check(all(v == 0 for v in read_mbw(d)), "blocks cleared")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48, help="registers per FC16 when writing the blocks (default 48)")
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
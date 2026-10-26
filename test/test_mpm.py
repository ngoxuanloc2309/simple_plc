#!/usr/bin/env python3
"""
test_mpm.py - Verify the MB_READ blocks 0x1000..0x10FF of SimplePLC (decisions 38..41
in docs/handoff.md, plc_mbm) over Modbus RTU.

Requires: pip install pymodbus pyserial
Firmware must be built with SPLC_MBM_ENABLE=1.

Standalone like test_tagtype.py, whose helpers it imports (same folder, which in turn
imports test_mqtt.py).

WARNING: this test WRITES Flash (rule table + MB_READ blocks), REPLACES the rule table and
runs CLEAR_RULES / FACTORY_RESET (which also wipe the MQTT config and the tag types).
Run it on a dev board only.

What it checks (no sensor needed): the CONFIGURATION path. Semantics (plc_mbm.h):
  * FC16/FC06 into 0x1000..0x10FF go to a DRAFT; reads return the RUNNING blocks.
  * COMMIT validates every drafted block first. A bad block (slave outside 1..247, FC
    outside 1..4, count outside 1..125, timeout < 10 ms, cyclic interval < 10 ms, pin
    count > 4, a pin target that is DI/DO/AI/COUNTER or beyond the tags, a data type
    outside 1..6, endianness > 3, a pin that does not fit in the count, a 32-bit type on
    a coil read, a trigger tag that does not exist, read mode > 1) rejects the WHOLE
    COMMIT (CONFIG_STATUS != READY, error 2); the running blocks do not change and the
    draft is dropped.
  * A block with the enable bit clear is not checked (the App writes zeros into unused
    blocks).
  * A Deploy that does not write the blocks keeps them running.
  * FC06 changes one register; the rest of the block is kept.
  * Only a request that runs past 0x10FF answers exception 0x02.
  * The blocks survive REBOOT; CLEAR_RULES and FACTORY_RESET clear them, CLEAR_RETAIN
    does not.
  * 0x0A08 (when readable) announces MODBUS_MASTER = 0x100.

Optional --live SLAVE: enables block 0 as FC03 of --live-count registers at --live-addr
from that slave every 500 ms into VREG tags and reports what the firmware did. With no
sensor on the bus the poll must fail with error=1 / err_code=1 (timeout); with a sensor
it must succeed (error=0 / err_code=0). Either outcome is consistent and is reported;
inconsistent status tags fail. This also needs the host to have registered an RS485
port with splc_mbm_set_rs485_port() (no port = no poll = a clear message).

Usage:
    python test_mpm.py COM14 --unit 1 --baudrate 115200
    python test_mpm.py COM14 --reboot [--port-after COM15]
    python test_mpm.py COM14 --live 1 --live-addr 0 --live-count 2 [--rs485-port 3 --baud 9600 --parity N]

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port,
3 = link error (inconclusive).
"""
import argparse
import test_report as R   # Excel evidence report (--report / --no-report)
import struct
import sys
import time

import test_mqtt as T
import test_tagtype as TT
from test_mqtt import check, info, LinkError

REG_MBM = 0x1000
BLOCKS, BLOCK_REGS = 4, 64
MBM_REGS = BLOCKS * BLOCK_REGS
REG_CAPS = 0x0A08
CAP_MODBUS_MASTER = 0x100
NONE = 0xFFFF

DT_BOOL, DT_INT16, DT_UINT16, DT_INT32, DT_UINT32, DT_REAL = 1, 2, 3, 4, 5, 6


# ------------------------------------------------------------------ encoding
def f32_regs(f):
    u = struct.unpack(">I", struct.pack(">f", f))[0]
    return [u >> 16, u & 0xFFFF]


def pin(tag=NONE, off=0, dt=DT_INT16, endian=0, mult=1.0):
    return [tag, off, dt, endian, *f32_regs(mult), 0, 0, 0, 0, 0, 0]


DEFAULT_PORT = None   # RS485 port index (0..3) given to every enabled block that names no port;
                      # None = leave the port bits 0 (port 1). test_mbm_live.py sets it.


def block(enabled=True, fc=3, slave=1, start=0, count=2, interval=500, timeout=200,
          valid=NONE, err=NONE, errcode=NONE, mode=0, trig=NONE, pins=(), port=None):
    """64-register image of one MB_READ block (layout of plc_mbm.h).
    port = RS485 port index 0..3 (App's port-1), flags bits 4..7; None -> DEFAULT_PORT."""
    r = [0] * BLOCK_REGS
    r[0] = 1 if enabled else 0
    if port is None:
        port = DEFAULT_PORT
    if enabled and port:
        r[0] |= (port & 0xF) << 4
    r[1] = (fc << 8) | slave
    r[2], r[3] = start, count
    r[4], r[5] = interval >> 16, interval & 0xFFFF
    r[6], r[7] = timeout >> 16, timeout & 0xFFFF
    r[8], r[9], r[10] = valid, err, errcode
    r[11] = len(pins)
    r[12], r[13] = mode, trig
    ps = list(pins) + [pin()] * (4 - len(pins))
    for i, p in enumerate(ps):
        r[16 + 12 * i:16 + 12 * (i + 1)] = p
    return r


READ_CHUNK = 64     # FC03 allows at most 125 registers per request; one block per read


def read_mbm(d):
    out = []
    for off in range(0, MBM_REGS, READ_CHUNK):
        out += d.read(REG_MBM + off, READ_CHUNK)
    return out


def try_read_mbm(d):
    """Like Dev.try_read() for the whole area, in chunks: (registers, None) or (None, error)."""
    out = []
    for off in range(0, MBM_REGS, READ_CHUNK):
        regs, err = d.try_read(REG_MBM + off, READ_CHUNK)
        if err is not None:
            return None, err
        out += regs
    return out, None


def image(blocks):
    """256-register image from {index: block}."""
    img = [0] * MBM_REGS
    for i, b in blocks.items():
        img[i * BLOCK_REGS:(i + 1) * BLOCK_REGS] = b
    return img


def write_blocks(d, blocks):
    """FC16 of each given block into the draft; returns the first error response or None."""
    for i, b in blocks.items():
        resp = d.write_regs(REG_MBM + i * BLOCK_REGS, b)
        if resp is not None:
            return resp
    return None


REG_PORTS = 0x0A0A          # RS485_PORT_COUNT
REG_PORT_TABLE = 0x1400     # 4 records x 16 registers (SPLC-SPEC-MBM-003)
PORT_REGS, PORT_TABLE_REGS = 16, 64
PARITY = {"N": 0, "E": 1, "O": 2}


def write_port_table(d, port_no, baud=9600, parity="N", stop=1):
    """Write the record of RS485 port `port_no` (1..4) into the draft: enabled, baud, 8 bits,
    parity, stop bits (same record test_mbm_port3.py writes). The other records are kept.
    Must be followed by a COMMIT in the same draft. Returns True when the draft write was ACKed."""
    idx = port_no - 1
    n, err = d.try_read(REG_PORTS, 1)
    if err is None:
        info(f"RS485_PORT_COUNT (0x0A0A) = {n[0]}")
        if not check(port_no <= n[0], f"the device has RS485 port {port_no} (it has {n[0]})"):
            info("SPLC_MBM_PORT_COUNT in splcopts.h is too small, or the host did not build with it")
            return False
    table = d.read(REG_PORT_TABLE, PORT_TABLE_REGS)
    new, b = list(table), idx * PORT_REGS
    new[b + 0] = 0x0100 | port_no
    new[b + 1], new[b + 2] = baud >> 16, baud & 0xFFFF
    new[b + 3] = PARITY[parity]
    new[b + 4] = (8 << 8) | stop
    new[b + 5], new[b + 6], new[b + 7] = 200, 20, 1
    resp = d.write_regs(REG_PORT_TABLE, new)
    return check(resp is None, f"port table: RS485 port {port_no} enabled at {baud} baud {parity}{stop} (draft)")


def deploy(d, rule, blocks):
    """Optional block write, then stage + COMMIT like the App. Returns (status, error, resp)."""
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


# ------------------------------------------------------------------ the test
def run(d, args):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} "
          f"RETAIN={retain} COUNTER={counter}")
    if vreg < 8 or vflag < 1:
        print("Need >= 8 VREG and >= 1 VFLAG tags (dense layout DI, DO, AI, VFLAG, VREG, RETAIN, COUNTER)")
        return
    di0, do0 = 0, di
    ai0 = di + do
    vf0 = ai0 + ai
    vr0 = vf0 + vflag
    ct0 = vr0 + vreg + retain
    total = ct0 + counter
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)
    TAGV = vr0 + 1          # target / status tags (plain VREGs)

    # ---------------------------------------------------------------- 0
    print("\n0. Probe")
    regs, err = try_read_mbm(d)
    if err is not None and getattr(err, "link", False):
        raise LinkError(f"probe 0x1000: {err}")
    if err is not None:
        print(f"  0x1000 not readable ({err}): firmware built with SPLC_MBM_ENABLE=0?")
        check(False, "MB_READ area readable")
        return
    check(len(regs) == MBM_REGS, "0x1000..0x10FF readable (256 registers)")
    # NOTE: an unmapped register reads 0 (it does not raise), so "readable" proves nothing
    # about the build. The capability bit is the real probe, and it is fatal: without it
    # every later check would only be measuring a firmware that has no MB_READ.
    caps, cerr = d.try_read(REG_CAPS, 2)
    if cerr is None:
        v = (caps[0] << 16) | caps[1]
        if not check(v & CAP_MODBUS_MASTER != 0, f"0x0A08 announces MODBUS_MASTER (caps 0x{v:08X})"):
            print("  This image has no MB_READ: build with SPLC_MBM_ENABLE 1 in splcopts.h "
                  "(splc_config/splcopts.h does not set it, the default is 0), do a clean rebuild "
                  "and flash it. Stopping: the other checks would be meaningless.")
            return
    else:
        info("0x0A08 not readable on this build (SPLC_MQTT_ENABLE=0 and SPLC_MBM_ENABLE=0?): "
             "writes below will show whether MB_READ exists")

    # ---------------------------------------------------------------- 1
    print("\n1. Clean start: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
    check(all(v == 0 for v in read_mbm(d)), "all 256 registers read 0 (every block disabled)")

    # ---------------------------------------------------------------- 2
    print("\n2. Draft invisible before COMMIT, byte-exact after it")
    b0 = block(fc=3, slave=5, start=100, count=8, interval=1000, timeout=300,
               valid=TAGV + 1, err=TAGV + 2, errcode=TAGV + 3,
               pins=[pin(TAGV, 0, DT_INT16), pin(TAGV + 4, 2, DT_REAL, endian=1, mult=0.1),
                     pin(vf0, 4, DT_BOOL), pin(TAGV + 5, 6, DT_UINT32, endian=3)])
    b1 = block(fc=4, slave=7, start=0, count=125, interval=250, timeout=1000, pins=[pin(TAGV, 124, DT_UINT16)])
    b2 = block(fc=1, slave=9, start=0, count=16, interval=100, timeout=100, mode=1, trig=vf0,
               pins=[pin(vf0, 3, DT_BOOL)])
    want = {0: b0, 1: b1, 2: b2}
    resp = write_blocks(d, want)
    check(resp is None, "FC16 of three blocks accepted")
    check(all(v == 0 for v in read_mbm(d)), "read before COMMIT still returns the RUNNING blocks (empty)")
    exp = {}
    if deploy_ok(d, rule, None, "Deploy"):
        exp = dict(want)
    got = read_mbm(d)
    check(got == image(exp), f"blocks read back byte-exact {TT.diff(got, image(exp)) or ''}")

    # ---------------------------------------------------------------- 3
    print("\n3. A Deploy that does not write the blocks keeps them")
    deploy_ok(d, rule, None, "Deploy without the blocks")
    check(read_mbm(d) == image(exp), "blocks unchanged")

    # ---------------------------------------------------------------- 4
    print("\n4. FC06 changes one register, the rest of the block is kept")
    new_iv = 2000
    resp = d.fc06(REG_MBM + 5, new_iv)         # block 0, interval low word (+5)
    check(not resp.isError(), "FC06 into the area accepted")
    check(read_mbm(d) == image(exp), "before COMMIT the running blocks are unchanged")
    if deploy_ok(d, rule, None, "Deploy"):
        exp[0] = list(exp[0])
        exp[0][5] = new_iv
    check(read_mbm(d) == image(exp), "after COMMIT: the new interval AND the rest of the block")

    # ---------------------------------------------------------------- 5
    print("\n5. COMMIT rejections are all-or-nothing, the draft is dropped")

    def bad(name, applicable=True, **kw):
        if not applicable:
            info(f"{name}: board has no such tag, skipped")
            return
        pins = kw.pop("pins", [pin(TAGV, 0, DT_INT16)])
        kw.setdefault("count", 4)
        base = block(pins=pins, **kw)
        # a GOOD change to block 3 in the same draft must not be applied either
        good = block(slave=2, count=2, pins=[pin(TAGV, 0, DT_INT16)])
        deploy_rejected(d, rule, {3: good, 2: base}, name)
        check(read_mbm(d) == image(exp), f"{name}: running blocks unchanged (the good block was not applied)")

    bad("slave 0", slave=0)
    bad("slave 248", slave=248)
    bad("function code 5", fc=5)
    bad("function code 0", fc=0)
    bad("count 0", count=0)
    bad("count 126", count=126)
    bad("timeout 5 ms", timeout=5)
    bad("cyclic interval 5 ms", interval=5)
    bad("pin target = DI", di > 0, pins=[pin(di0, 0)])
    bad("pin target = DO", do > 0, pins=[pin(do0, 0)])
    bad("pin target = AI", ai > 0, pins=[pin(ai0, 0)])
    bad("pin target = COUNTER", counter > 0, pins=[pin(ct0, 0)])
    bad("pin target beyond the tags", pins=[pin(0x7FFF, 0)])
    bad("status tag = DI", di > 0, valid=di0, pins=[pin(TAGV, 0)])
    bad("pin offset outside the count", pins=[pin(TAGV, 4)])
    bad("data type 7", pins=[pin(TAGV, 0, 7)])
    bad("data type 0", pins=[pin(TAGV, 0, 0)])
    bad("endianness 4", pins=[pin(TAGV, 0, DT_INT16, 4)])
    bad("REAL does not fit (offset 3, count 4)", pins=[pin(TAGV, 3, DT_REAL)])
    bad("32-bit type on a coil read", fc=1, pins=[pin(TAGV, 0, DT_INT32)])
    bad("read mode 2", mode=2)
    bad("on-trigger without a trigger tag", mode=1, trig=NONE)
    bad("trigger tag beyond the tags", mode=1, trig=0x7FFF)
    # pin count > 4 cannot be built by block(); poke the register
    bpc = block(count=4, pins=[pin(TAGV, 0)])
    bpc[11] = 5
    deploy_rejected(d, rule, {2: bpc}, "pin count 5")
    check(read_mbm(d) == image(exp), "pin count 5: running blocks unchanged")
    deploy_ok(d, rule, None, "Deploy after the rejections")
    check(read_mbm(d) == image(exp), "draft was dropped by the rejected COMMITs (nothing leaks into the next one)")

    # a disabled block is not checked; it also disables a running one
    junk = [0xABAB] * BLOCK_REGS
    junk[0] = 0
    if deploy_ok(d, rule, {2: junk}, "disabled block with garbage content"):
        exp[2] = junk
    check(read_mbm(d) == image(exp), "the disabled block is stored as written (block 2 is off)")

    # ---------------------------------------------------------------- 6
    print("\n6. Address limits")
    last = read_mbm(d)[MBM_REGS - 1]
    resp = d.fc16(REG_MBM + MBM_REGS - 1, [last, 0])
    check(resp.isError() and T.exc_code(resp) == 0x02, "FC16 running past 0x10FF -> exception 0x02")
    check(read_mbm(d) == image(exp), "...and nothing changed")
    resp = d.fc16(REG_MBM + MBM_REGS - 1, [last])
    check(not resp.isError(), "FC16 of the very last register accepted")
    resp = d.fc16(REG_MBM - 1, [0, 0])
    check(resp.isError() and T.exc_code(resp) == 0x02, "FC16 starting below 0x1000 and running in -> exception 0x02")
    deploy_ok(d, rule, None, "Deploy (drops the draft)")
    check(read_mbm(d) == image(exp), "blocks unchanged after the address tests")

    # ---------------------------------------------------------------- 7
    if args.reboot:
        print("\n7. Persistence across REBOOT and the clear commands")
        nd = d.reboot(args)
        if nd is None:
            check(False, "board came back after REBOOT")
            return
        d = nd
        check(read_mbm(d) == image(exp), "blocks identical after REBOOT (restored from Flash)")
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RETAIN)
        check(s == T.ST_DONE and e == 0, f"CLEAR_RETAIN result {s}/{e}")
        check(read_mbm(d) == image(exp), "CLEAR_RETAIN keeps the blocks")
        for name, cmd in (("CLEAR_RULES", T.SYS_CLEAR_RULES), ("FACTORY_RESET", T.SYS_FACTORY)):
            deploy_ok(d, rule, exp, f"re-deploy the blocks before {name}")
            s, e = T.run_syscmd(d, cmd)
            check(s == T.ST_DONE and e == 0, f"{name} result {s}/{e}")
            check(all(v == 0 for v in read_mbm(d)), f"{name} clears the blocks")
            nd = d.reboot(args)
            if nd is None:
                check(False, "board came back after REBOOT")
                return
            d = nd
            check(all(v == 0 for v in read_mbm(d)), f"...and they stay clear after REBOOT ({name} was saved)")
        exp = {}
    else:
        print("\n7. (skipped: add --reboot for persistence and the clear commands)")

    # ---------------------------------------------------------------- 8
    if args.live is not None:
        print(f"\n8. Live poll of slave {args.live} on RS485 port {args.rs485_port} "
              f"(FC03, {args.live_count} register(s) at {args.live_addr}, {args.baud} baud {args.parity}{args.stop})")
        valid, errt, code, t0v = TAGV + 1, TAGV + 2, TAGV + 3, TAGV
        lb = block(fc=3, slave=args.live, start=args.live_addr, count=args.live_count,
                   interval=500, timeout=300, valid=valid, err=errt, errcode=code,
                   pins=[pin(t0v, 0, DT_UINT16)], port=args.rs485_port - 1)
        if not write_port_table(d, args.rs485_port, args.baud, args.parity, args.stop):
            return
        deploy_ok(d, rule, {0: lb}, f"enable block 0 on RS485 port {args.rs485_port}")
        seen_valid, last = False, None
        t_end = time.time() + 4.0
        while time.time() < t_end:
            vals = [TT.tag_value(d, t) for t in (valid, errt, code, t0v)]
            seen_valid = seen_valid or vals[0] == 1
            last = vals
            time.sleep(0.05)
        v, e, c, x = last
        info(f"last status: valid={v} error={e} err_code={c} value={x}; valid pulse seen: {seen_valid}")
        if e == 0:
            check(c == 0 and seen_valid, "sensor answered: err_code 0 and the valid pulse was seen")
        else:
            check(e == 1 and 1 <= c <= 4, f"poll failed consistently: error=1, err_code={c} (1 timeout, 2 CRC, 3 exception, 4 malformed)")
            if c == 1:
                info("timeout: no sensor on the bus, wrong slave/baud, or no RS485 port registered by the host")
        deploy_ok(d, rule, {0: block(enabled=False)}, "disable block 0 again")
    else:
        print("\n8. (skipped: add --live SLAVE for a live poll)")

    # ---------------------------------------------------------------- 9
    print("\n9. Cleanup (CLEAR_RULES)")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
    check(all(v == 0 for v in read_mbm(d)), "blocks cleared")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48,
                   help="registers per FC16 when writing the blocks (default 48)")
    p.add_argument("--reboot", action="store_true", help="also check persistence across REBOOT")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    p.add_argument("--live", type=int, default=None, metavar="SLAVE",
                   help="also poll this slave id with block 0 and check the status tags")
    p.add_argument("--live-addr", type=int, default=0)
    p.add_argument("--rs485-port", type=int, choices=[1, 2, 3, 4], default=3,
                   help="RS485 port the --live poll uses (default 3 = USART3 of the wg_ue gateway)")
    p.add_argument("--baud", type=int, default=9600, help="line speed of that port for --live (default 9600)")
    p.add_argument("--parity", choices=["N", "E", "O"], default="N")
    p.add_argument("--stop", type=int, choices=[1, 2], default=1)
    p.add_argument("--live-count", type=int, default=2)
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
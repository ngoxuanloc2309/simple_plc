#!/usr/bin/env python3
"""
test_mbm_port2.py - Does the device really poll a slave on RS485 port N (default 2),
without going through Studio? It does what Studio's Deploy does for ONE MB_READ block:

    1. reads RS485_PORT_COUNT (0x0A0A) and the port table 0x1400
    2. writes the port record of port N (enabled, baud, 8 bits, parity, 1 stop)
    3. writes the REAL tag types, block 0 (port N, FC, slave, 4 pins) and COMMITs
    4. watches the target tags and the three status tags for --seconds

Needs two serial ports, like test_mbm_live.py:
    * the board's USB-CDC port (this script)         e.g. COM14
    * a USB-RS485 dongle on the board's RS485 port N e.g. COM20 (mfm383a_sim.py)

Usage (start the simulator first, in another terminal, and keep --trace on to SEE the
board's request arrive on the dongle - that is the proof the board sent it on port N):

    python mfm383a_sim.py COM20 --unit 1 --baud 9600 --trace
    python test_mbm_port2.py COM14                          # MFM383A: FC04, 0x00, 4 REAL (V1N..AvgLN)
    python test_mbm_port2.py COM14 --rs485-port 2 --slave 3 --baud 19200 --parity E
    python test_mbm_port2.py COM14 --fc 4 --start 0x10 --count 8     # currents I1, I2, I3, AvgI
    python test_mbm_port2.py COM14 --endian 1                         # if the floats look wrong
    python test_mbm_port2.py COM14 --dtype uint16 --fc 3 --count 4        # against mbm_slave_sim.py

What the result means:
    ALL VALUES OK       the board polled port N, got a valid answer, wrote the tags
    COMMIT rejected     the device has fewer RS485 ports than N (see "RS485_PORT_COUNT"),
                        or a tag is not writable / not enough VREG tags
    err_code 1          timeout: nothing came back. The sim shows no "RX" line  -> the board did
                        NOT send on that port (host did not register port N with
                        splc_mbm_set_rs485_port(), wrong wiring A/B, or the port is disabled);
                        the sim shows "RX" but the board saw nothing -> slave id / baud / parity
    err_code 2/3/4      the bus works (CRC / exception / malformed answer)

WARNING: this WRITES Flash (the port table, the tag types, MB_READ block 0, one dummy rule) and
REPLACES the rule table. Dev board only. The polling is LEFT RUNNING when the script ends (so the
sim keeps seeing requests); add --clear to end with CLEAR_RULES.
Firmware: SPLC_MBM_ENABLE=1, SPLC_REAL_ENABLE=1, SPLC_MBM_PORT_COUNT >= N.

Exit codes: 0 = values read, 1 = a check failed / no valid poll, 2 = cannot open port, 3 = link error.
"""
import argparse
import struct
import sys
import time

import test_mqtt as T
import test_tagtype as TT
import test_mpm as M
from test_mqtt import check, info, LinkError

REG_PORTS = 0x0A0A
REG_PORT_TABLE = 0x1400
PORT_REGS, PORT_TABLE_REGS = 16, 64
PARITY = {"N": 0, "E": 1, "O": 2}
DTYPES = {"real": M.DT_REAL, "uint16": M.DT_UINT16, "int16": M.DT_INT16,
          "uint32": M.DT_UINT32, "int32": M.DT_INT32}
STRIDE = {"real": 2, "uint16": 1, "int16": 1, "uint32": 2, "int32": 2}
NAMES = ["V1N", "V2N", "V3N", "AvgVLN"]


def as_float(bits):
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


def show(dtype, bits):
    if dtype == "real":
        return f"{as_float(bits):.3f}"
    if dtype in ("int16", "int32"):
        width = 16 if dtype == "int16" else 32
        v = bits & ((1 << width) - 1)
        return str(v - (1 << width) if v >> (width - 1) else v)
    return str(bits & 0xFFFFFFFF)


def u16(s):
    return int(s, 0)


def run(d, args):
    idx = args.rs485_port - 1                           # wire index 0..3
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 8:
        print("Need >= 8 VREG tags (4 targets + 3 status + 1 dummy)")
        T._failures += 1
        return None
    vr0 = di + do + ai + vflag
    tgt = [vr0 + 1 + i for i in range(args.pins)]
    valid_t, err_t, code_t = vr0 + 5, vr0 + 6, vr0 + 7
    if args.pins > 4:
        print("--pins is at most 4 (a block has 4 pin slots)")
        T._failures += 1
        return None

    print("\n1. Device capabilities")
    caps, cerr = d.try_read(M.REG_CAPS, 2)
    if cerr is None:
        v = (caps[0] << 16) | caps[1]
        check(v & M.CAP_MODBUS_MASTER != 0, f"0x0A08 announces MODBUS_MASTER (caps 0x{v:08X})")
        if args.dtype == "real":
            check(v & 0x800 != 0, "0x0A08 announces FLOAT32 (bit 0x0800)")
    ports, perr = d.try_read(REG_PORTS, 1)
    if perr is None:
        n_ports = ports[0]
        info(f"RS485_PORT_COUNT (0x0A0A) = {n_ports}")
        if not check(args.rs485_port <= n_ports,
                     f"the device has RS485 port {args.rs485_port} (it has {n_ports})"):
            info("SPLC_MBM_PORT_COUNT in splcopts.h is too small, or the host did not build with it")
            return None
    else:
        info(f"0x0A0A not readable ({perr}): old firmware, assuming 1 port")

    print(f"\n2. Port table 0x1400: port {args.rs485_port}")
    table = d.read(REG_PORT_TABLE, PORT_TABLE_REGS)
    rec = table[idx * PORT_REGS:(idx + 1) * PORT_REGS]
    info(f"before: control=0x{rec[0]:04X} (enabled={bool(rec[0] & 0x100)}) baud={(rec[1] << 16) | rec[2]} "
         f"parity={rec[3]} framing=0x{rec[4]:04X} timeout={rec[5]} gap={rec[6]} retries={rec[7]}")
    new = list(table)
    b = idx * PORT_REGS
    new[b + 0] = 0x0100 | (idx + 1)
    new[b + 1], new[b + 2] = args.baud >> 16, args.baud & 0xFFFF
    new[b + 3] = PARITY[args.parity]
    new[b + 4] = (8 << 8) | args.stop
    new[b + 5], new[b + 6], new[b + 7] = 200, 20, 1
    resp = d.write_regs(REG_PORT_TABLE, new)
    if not check(resp is None, "port table written into the draft"):
        return None

    print("\n3. Deploy: tag types, MB_READ block 0, COMMIT")
    stride = STRIDE[args.dtype]
    if args.pins * stride > args.count:
        print(f"--count {args.count} is too small for {args.pins} {args.dtype} pin(s) (needs {args.pins * stride})")
        T._failures += 1
        return None
    pins = [M.pin(tgt[i], i * stride, DTYPES[args.dtype], args.endian, 1.0) for i in range(args.pins)]
    blk = M.block(enabled=True, fc=args.fc, slave=args.slave, start=args.start, count=args.count,
                  interval=args.interval, timeout=args.timeout, valid=valid_t, err=err_t, errcode=code_t,
                  pins=pins)
    blk[0] |= idx << 4                                   # F_FLAGS: bits 4..7 = RS485 port index
    resp = TT.write_types(d, {t: TT.REAL for t in tgt} if args.dtype == "real" else {})
    if not check(resp is None, "tag type table written"):
        return None
    full = {i: M.block(enabled=False) for i in range(M.BLOCKS)}
    full[0] = blk
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)
    if not M.deploy_ok(d, rule, full, f"block 0 on RS485 port {args.rs485_port}"):
        info("see the device log: 'block 0: port N, the device has M' / 'target tag ... not writable'")
        return None
    rec = d.read(REG_PORT_TABLE + idx * PORT_REGS, PORT_REGS)
    check(bool(rec[0] & 0x100) and ((rec[1] << 16) | rec[2]) == args.baud,
          f"port {args.rs485_port} reads back enabled at {(rec[1] << 16) | rec[2]} baud")

    print(f"\n4. Watching the tags for {args.seconds:.0f} s (slave {args.slave}, FC{args.fc:02d}, "
          f"0x{args.start:04X} x{args.count})")
    first = vr0
    seen_valid, last, changes, prev = False, None, 0, None
    samples = 0
    t_end = time.time() + args.seconds
    next_print = 0.0
    while time.time() < t_end:
        raw = d.read(T.REG_TAGS + 2 * first, 16)
        tags = [(raw[2 * i] << 16) | raw[2 * i + 1] for i in range(8)]
        vals = tags[1:1 + args.pins]
        valid, err, code = tags[5], tags[6], tags[7]
        seen_valid = seen_valid or valid == 1
        if prev is not None and vals != prev:
            changes += 1
        prev, last = vals, (valid, err, code, vals)
        samples += 1
        now = time.time()
        if now >= next_print:
            next_print = now + 1.0
            shown = ", ".join(f"{(NAMES[i] if args.dtype == 'real' and args.fc == 4 and args.start == 0 and args.count >= 8 else f'p{i}')}="
                              f"{show(args.dtype, v)}" for i, v in enumerate(vals))
            print(f"   valid={valid} error={err} err_code={code}  {shown}")
        time.sleep(0.05)

    valid, err, code, vals = last
    print("\n5. Verdict")
    info(f"{samples} samples, valid pulse seen: {seen_valid}, the values changed {changes} time(s)")
    if err == 0 and code == 0 and any(v != 0 for v in vals):
        check(True, f"ALL VALUES OK: the board polled RS485 port {args.rs485_port} and wrote the tags "
                    f"({', '.join(show(args.dtype, v) for v in vals)})")
    elif err == 0 and code == 0:
        check(False, "poll reports success but every target tag is 0 (wrong start address / slave map?)")
    else:
        why = {1: "timeout: no valid answer reached the board", 2: "CRC error in the answer",
               3: "the slave answered a Modbus exception", 4: "malformed / wrong-size answer"}.get(code, "?")
        check(False, f"poll failed: error={err} err_code={code} ({why})")
        if code == 1:
            info("compare with the simulator window:")
            info("  * NO 'RX' line there  -> the board did not transmit on this port:")
            info("      host never called splc_mbm_set_rs485_port() for it, A/B wiring, or the port is disabled")
            info("  * 'RX' lines but still timeout -> slave id, baud/parity, or the answer is later than --timeout")
        if code == 3:
            info("exception: start/count outside the slave's map, or FC not supported there")
    return vr0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="board USB-CDC port, e.g. COM14")
    p.add_argument("--unit", type=int, default=1, help="Modbus unit of the board's own config port (default 1)")
    p.add_argument("--baudrate", type=int, default=115200, help="baud of the USB-CDC port")
    p.add_argument("--chunk", type=int, default=48)
    p.add_argument("--rs485-port", type=int, default=2, choices=[1, 2, 3, 4],
                   help="RS485 port to poll, 1-based like Studio (default 2)")
    p.add_argument("--slave", type=int, default=1, help="slave id of the meter / sim (default 1)")
    p.add_argument("--baud", type=int, default=9600, help="line speed of the RS485 port (default 9600)")
    p.add_argument("--parity", choices=["N", "E", "O"], default="N")
    p.add_argument("--stop", type=int, choices=[1, 2], default=1)
    p.add_argument("--fc", type=int, choices=[1, 2, 3, 4], default=4, help="function code (default 4)")
    p.add_argument("--start", type=u16, default=0, help="start address, e.g. 0x10 (default 0)")
    p.add_argument("--count", type=int, default=8, help="registers to read (default 8)")
    p.add_argument("--pins", type=int, default=4, help="pins to map, 1..4 (default 4)")
    p.add_argument("--dtype", choices=list(DTYPES), default="real", help="pin data type (default real)")
    p.add_argument("--endian", type=int, choices=[0, 1, 2, 3], default=0,
                   help="0 ABCD, 1 CDAB, 2 BADC, 3 DCBA (default 0)")
    p.add_argument("--interval", type=int, default=500, help="poll interval ms (default 500)")
    p.add_argument("--timeout", type=int, default=300, help="answer timeout ms (default 300)")
    p.add_argument("--seconds", type=float, default=6.0, help="how long to watch the tags (default 6)")
    p.add_argument("--clear", action="store_true", help="end with CLEAR_RULES (stops the polling)")
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
        if args.clear:
            print("\nCleanup (CLEAR_RULES)")
            s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
            check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
        else:
            print("\n(the polling keeps running; run again with --clear, or CLEAR_RULES, to stop it)")
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
    if link_error is not None:
        print(f"\nINCONCLUSIVE: a request outcome was unknown (link error). {T._failures} check(s) failed before it.")
        sys.exit(3)
    print("\nALL PASS" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
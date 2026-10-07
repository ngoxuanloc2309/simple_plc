#!/usr/bin/env python3
"""
test_rtc.py - Verify the RTC block (0x0810..0x0813) and TRG_TIME_WINDOW rules
(Wire Profile V2.0, plan step 7) over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial   (same as the other test scripts)
Put this file next to test_plc.py and test_sysclear.py (it reuses their
helpers).

Board-agnostic: tag indices come from DeviceResourceInfo (0x0020), dense
layout (DI, DO, AI, VFLAG, VREG, VREG_RETAIN, COUNTER) like plc_tag.c.

WARNING: this test uploads rules (WRITES Flash, replaces the rule table) and
finishes with CLEAR_RULES, so any rules already on the board are lost. It also
SETS the board clock to fake times (e.g. 08:29:55, 23:59:58) and leaves it at
the PC's real time. Do not run it on a board whose rules you want to keep.

Time model being tested (docs/SimplePLC_App_MCU_Structs_v2.0 sections 3.4, 7):
  0x0810..0x0811 epoch_utc_s (u32, High Word first)   Host writes
  0x0812         tz_offset_min (i16)                  Host writes
  0x0813         status_flags                         READ-ONLY, board-computed
  SYNCED=0x0001 HW_PRESENT=0x0002 BATTERY_LOW=0x0004. This board: internal RTC
  on LSI, no crystal, no battery -> HW_PRESENT=0, BATTERY_LOW=0 always.

Checks, in order:
  1. Read block shape, HW_PRESENT/BATTERY_LOW are 0.
  2. Write 4 regs with the Host's status_flags = 0x0003 -> accepted, read back
     epoch/tz, flags == SYNCED only (the Host's value is ignored). Write of 3
     regs (epoch + tz) also accepted.
  3. Rejections, clock UNCHANGED afterwards: epoch outside 2000..2099 (0x03),
     tz outside -720..+840 (0x03), FC06 (0x03), qty 2 (0x03), write to 0x0813
     only (0x02), write starting at 0x0812 (0x02).
  4. The clock runs at ~1 s/s (LSI, +-several %: reported, loose limits).
  5. Exact-minute rule (EQ, Lo=Hi=830, INC VREG0): clock set to 08:29:55 local
     -> counter stays 0 before 08:30, becomes exactly 1 after, stays 1.
  6. Window rule (Lo=830, Hi=1700, INC VREG1): counts continuously from 08:30.
  7. Window across midnight (Lo=2358, Hi=1, INC VREG2): clock set to 23:59:58
     -> counter keeps rising before AND after 00:00.
  8. Cleanup: CLEAR_RULES, clock back to PC time.

Optional:
  --reboot  After setting the time, REBOOT: SYNCED and the epoch must SURVIVE
            (the --wrap in simple_plc/CMakeLists.txt), tz_offset_min reads 0
            (RAM only), and the Time Window rule does NOT fire until the Host
            writes the time again. The COM port drops during reboot; if Windows
            renumbers it pass --port-after COMx.

NOT covered (needs a real power cut, do it by hand): unplug USB/power for a few
seconds, plug back, read 0x0810..0x0813: flags must read 0 (SYNCED=0, the RTC
has no VBAT) and a Time Window rule must not fire until the Host writes the
time again.

Usage:
  python test_rtc.py COM14
  python test_rtc.py COM14 --reboot
"""

import argparse
import datetime as dt
import sys
import time

from pymodbus.client import ModbusSerialClient

import test_plc as tp        # rule encoding / CRC helpers
import test_sysclear as ts   # Dev wrapper, check(), reboot_and_reconnect()

REG_RESOURCE = 0x0020
REG_RTC = 0x0810
REG_STATUS = 0x9000
REG_RULE_COUNT_STAGED = 0x9002
REG_EXPECTED_CRC = 0x9003
REG_STAGING = 0x9010
REG_COMMIT = 0xA000
COMMIT_MAGIC = 0xA5A5

FLAG_SYNCED, FLAG_HW_PRESENT, FLAG_BATTERY_LOW = 0x0001, 0x0002, 0x0004

OP_EQ = 1
ACT_INC_COUNTER = 2
TZ_VN = 420   # UTC+7

check = ts.check
exc_code = ts.exc_code


def local_epoch(y, mo, d, h, mi, s, tz_min):
    """UTC epoch for a LOCAL wall-clock time with the given tz offset."""
    utc = dt.datetime(y, mo, d, h, mi, s, tzinfo=dt.timezone.utc)
    return int(utc.timestamp()) - tz_min * 60


def rtc_read(d):
    r = d.read(REG_RTC, 4)
    tz = r[2] - 0x10000 if r[2] & 0x8000 else r[2]
    return (r[0] << 16) | r[1], tz, r[3]


def rtc_write(d, epoch, tz, flags=0x0003, qty=4):
    regs = [(epoch >> 16) & 0xFFFF, epoch & 0xFFFF, tz & 0xFFFF, flags][:qty]
    return d.fc16(REG_RTC, regs)


def set_time(d, epoch, tz=TZ_VN):
    resp = rtc_write(d, epoch, tz)
    if resp.isError():
        raise RuntimeError(f"time write failed: {resp}")


def time_rule(op, lo, hi, tag):
    """TIME_WINDOW rule: trigger_tag ignored, action = INC tag by 1."""
    return tp.encode_rule_registers(
        threshold_lo=lo, threshold_hi=hi, for_ms=0, action_param=1,
        trigger_tag=0, action_tag=tag, guard_tag=tp.GUARD_TAG_NONE,
        enabled=1, trigger_type=tp.SPLC_TRG_TIME_WINDOW,
        compare_op=op, action_type=ACT_INC_COUNTER)


def upload_rules(d, rules):
    """Stage + commit a list of rules (each a list of 16 registers)."""
    crc = tp.crc16_modbus(b"".join(tp.rule_registers_to_bytes(r) for r in rules))
    d.fc06(REG_RULE_COUNT_STAGED, len(rules))
    # One FC16 per rule (16 registers = 41-byte frame), like test_plc.py and
    # test_rule.py. A single FC16 carrying 2 rules is a 73-byte frame, longer
    # than one 64-byte USB CDC packet, and was not answered on the board.
    for i, rule in enumerate(rules):
        d.fc16(REG_STAGING + i * 16, rule)
    d.fc06(REG_EXPECTED_CRC, crc)
    d.fc06(REG_COMMIT, COMMIT_MAGIC)
    t0 = time.time()
    status = d.read(REG_STATUS, 1)[0]
    while status != 3 and time.time() - t0 < 3.0:   # 3 = READY
        time.sleep(0.1)
        status = d.read(REG_STATUS, 1)[0]
    return status == 3 and d.rule_count() == len(rules)


def counters(d, base):
    return d.tag(base), d.tag(base + 1), d.tag(base + 2)


def run(d, holder, args):
    ts.ensure_running(d)
    r = d.read(REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    print(f"resources: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 3:
        print("need at least 3 VREG tags"); sys.exit(2)
    v0 = di + do + ai + vflag     # first VREG tag index (dense layout)
    print(f"VREG0 = tag {v0}")

    # ---- 1. block shape ---------------------------------------------------
    print("\n[1] read block")
    epoch, tz, flags = rtc_read(d)
    print(f"  epoch={epoch} tz={tz} flags=0x{flags:04X}")
    check(not flags & FLAG_HW_PRESENT, "HW_PRESENT = 0 (internal LSI RTC)")
    check(not flags & FLAG_BATTERY_LOW, "BATTERY_LOW = 0")

    # ---- 2. write / readback ----------------------------------------------
    print("\n[2] write 4 registers (Host sends status_flags=0x0003)")
    now = int(time.time())
    resp = rtc_write(d, now, TZ_VN, flags=0x0003, qty=4)
    check(not resp.isError(), "FC16 qty 4 accepted")
    epoch, tz, flags = rtc_read(d)
    check(0 <= epoch - now <= 2, f"epoch read back (+{epoch - now} s)")
    check(tz == TZ_VN, f"tz read back = {tz}")
    check(flags == FLAG_SYNCED,
          f"flags == SYNCED only (got 0x{flags:04X}; the Host's 0x0003 is ignored)")
    resp = rtc_write(d, now, -300, qty=3)
    check(not resp.isError(), "FC16 qty 3 (epoch + tz) accepted")
    check(rtc_read(d)[1] == -300, "tz -300 read back")
    set_time(d, now, TZ_VN)

    # ---- 3. rejections ----------------------------------------------------
    print("\n[3] rejections (clock must stay unchanged)")
    before = rtc_read(d)[:2]
    check(exc_code(rtc_write(d, 1, TZ_VN)) == 3, "epoch=1 -> exception 0x03")
    check(exc_code(rtc_write(d, 4102444800, TZ_VN)) == 3, "epoch 2100-01-01 -> 0x03")
    check(exc_code(rtc_write(d, now, 900)) == 3, "tz +900 -> 0x03")
    check(exc_code(rtc_write(d, now, -721)) == 3, "tz -721 -> 0x03")
    check(exc_code(d.fc06(REG_RTC, 5)) == 3, "FC06 to 0x0810 -> 0x03")
    check(exc_code(d.fc16(REG_RTC, [1, 2])) == 3, "FC16 qty 2 -> 0x03")
    check(exc_code(d.fc16(REG_RTC + 3, [0])) == 2, "write 0x0813 only -> 0x02")
    check(exc_code(d.fc16(REG_RTC + 2, [420, 0])) == 2, "write from 0x0812 -> 0x02")
    after = rtc_read(d)[:2]
    check(after[1] == before[1] and 0 <= after[0] - before[0] <= 2,
          "clock and tz unchanged after all rejections")

    # ---- 4. clock rate ----------------------------------------------------
    print("\n[4] clock rate over 5 s")
    e1 = rtc_read(d)[0]; t1 = time.time()
    time.sleep(5.0)
    e2 = rtc_read(d)[0]; t2 = time.time()
    dev = (e2 - e1) / (t2 - t1)
    print(f"  board advanced {e2 - e1} s in {t2 - t1:.2f} s real ({(dev - 1) * 100:+.1f} %)")
    check(4 <= e2 - e1 <= 6, "clock advances ~1 s/s (LSI: expect a few %; a 2.3 % "
                             "error means the wrong prescaler, 127/255 instead of 127/249)")

    # ---- 5/6. exact minute + window --------------------------------------
    print("\n[5/6] exact-minute and window rules, clock set to 08:29:55 local")
    rules = [time_rule(OP_EQ, 830, 830, v0),          # exact minute -> VREG0
             time_rule(0, 830, 1700, v0 + 1)]         # window       -> VREG1
    # Park the clock at 03:00 local first: the rules must not fire while they
    # are being committed (the window rule would, at the PC's real time).
    set_time(d, local_epoch(2026, 1, 15, 3, 0, 0, TZ_VN))
    check(upload_rules(d, rules), "2 Time Window rules committed")
    set_time(d, local_epoch(2026, 1, 15, 8, 29, 55, TZ_VN))
    time.sleep(0.5)
    c = counters(d, v0)
    check(c[0] == 0 and c[1] == 0, f"08:29:56 -> both counters 0 (got {c[:2]})")
    time.sleep(6.0)                                   # now ~08:30:02
    c = counters(d, v0)
    print(f"  at ~08:30:02: exact={c[0]} window={c[1]}")
    check(c[0] == 1, "exact-minute rule fired exactly once")
    check(c[1] > 50, "window rule counts every scan (level)")
    w1 = c[1]
    time.sleep(3.0)
    c = counters(d, v0)
    check(c[0] == 1, "exact-minute rule did NOT repeat within the minute")
    check(c[1] > w1, "window rule still counting")

    # ---- 7. across midnight ----------------------------------------------
    print("\n[7] window across midnight (Lo=2358, Hi=1), clock set to 23:59:58")
    set_time(d, local_epoch(2026, 1, 15, 12, 0, 0, TZ_VN))   # outside 23:58..00:01
    check(upload_rules(d, [time_rule(0, 2358, 1, v0 + 2)]), "midnight rule committed")
    # Volatile counters are not reset by a commit, so only deltas are used.
    set_time(d, local_epoch(2026, 1, 15, 23, 59, 58, TZ_VN))
    time.sleep(1.0)
    a = d.tag(v0 + 2)
    time.sleep(1.0)                                   # ~00:00:00
    b = d.tag(v0 + 2)
    time.sleep(3.0)                                   # ~00:00:03, still < 00:02
    c2 = d.tag(v0 + 2)
    check(b > a, "counting before midnight (23:59)")
    check(c2 > b, "still counting after midnight (00:00)")

    # ---- reboot persistence ----------------------------------------------
    if args.reboot:
        print("\n[R] reboot persistence")
        check(upload_rules(d, [time_rule(0, 0, 2359, v0)]), "all-day window rule committed")
        pc_now = int(time.time())
        set_time(d, pc_now)
        d2 = ts.reboot_and_reconnect(d, holder, args)
        if d2 is None:
            check(False, "board came back after REBOOT")
        else:
            d = d2
            epoch, tz, flags = rtc_read(d)
            check(flags == FLAG_SYNCED, f"SYNCED survives REBOOT (flags=0x{flags:04X})")
            elapsed = time.time() - pc_now
            check(0 <= epoch - pc_now <= elapsed + 3,
                  f"epoch survived and kept running (+{epoch - pc_now} s of ~{elapsed:.0f} s)")
            check(tz == 0, f"tz is RAM-only, reads 0 after reboot (got {tz})")
            time.sleep(0.5)
            a = d.tag(v0)
            time.sleep(1.0)
            check(d.tag(v0) == a,
                  "Time Window rule does NOT fire until the Host rewrites the time")
            set_time(d, int(time.time()))
            time.sleep(1.0)
            a = d.tag(v0)
            time.sleep(1.0)
            check(d.tag(v0) > a, "Time Window rule fires again after the Host writes time")

    # ---- 8. cleanup -------------------------------------------------------
    print("\n[8] cleanup: CLEAR_RULES + clock back to PC time")
    st, err, resp = d.run_syscmd(ts.SYS_CLEAR_RULES)
    check(st == ts.ST_DONE and err == ts.ERR_NONE, f"CLEAR_RULES done (st={st} err={err})")
    set_time(d, int(time.time()), TZ_VN)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--reboot", action="store_true",
                   help="also verify the time survives a REBOOT")
    p.add_argument("--port-after", default=None,
                   help="COM port to use after a reboot if Windows renumbers it")
    args = p.parse_args()

    client = ModbusSerialClient(port=args.port, baudrate=args.baudrate, parity="N",
                                stopbits=1, bytesize=8, timeout=1.0)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    holder = [client]
    try:
        run(ts.Dev(client, args.unit), holder, args)
    finally:
        try:
            holder[0].close()
        except Exception:
            pass
    fails = ts._failures
    print("\nALL PASS" if fails == 0 else f"\n{fails} CHECK(S) FAILED")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
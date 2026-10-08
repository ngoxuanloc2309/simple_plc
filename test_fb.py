#!/usr/bin/env python3
"""
test_fb.py - Verify the Function Block block (0x0B00..0x0B7F, Wire Profile
V2.0, plan step 8a) over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial   (same as the other test scripts)
Put this file next to test_plc.py, test_sysclear.py and test_rtc.py (it
reuses their helpers).

Board-agnostic: tag indices come from DeviceResourceInfo (0x0020), dense
layout (DI, DO, AI, VFLAG, VREG, VREG_RETAIN, COUNTER) like plc_tag.c.

WARNING: this test WRITES the FB config (RAM only in step 8a, nothing goes to
Flash yet), uploads rules (WRITES Flash, replaces the rule table), forces
COUNTER tags through DIAG, and finishes with CLEAR_RULES and every FB block
set back to "unused". Rules already on the board are lost. Do not run it on a
board whose rules you want to keep.

What step 8a does (see services/plc_fb/plc_fb.h):
  Host writes   Timer  : mode (+1), pt_ms (+2..+3)
                Counter: mode (+1), preset (+2..+3), retain_tag_index (+6)
  Firmware owns status_bits (+0), ET / CV (+4..+5), reserved. Whole-block
  writes (like the App's Deploy) are accepted and those fields are skipped.
  Counter CV is the tag COUNTER[i] (one value, two addresses); Q is derived.
  Timer status/ET read 0 (the timing runs in rules, not in firmware).

Checks, in order:
  1. Profile V2, resource counts, counter/retain tag bases.
  2. Defaults: after "unused" is written every block reads mode 0, retain 0xFFFF.
  3. Reading the block: 64 + 64 registers works. A single 128-register read is
     reported (Modbus allows at most 125 per FC03, so it is expected to be
     rejected; the App currently asks for 128 in one request).
  4. Timer whole-block write: config read back, 32-bit PT High Word first,
     junk in status/ET/reserved ignored (reads 0).
  5. Counter whole-block write: config read back incl. negative PV, retain index.
  6. CV follows the COUNTER tag written through DIAG; Q for CTU (CV >= PV) and
     CTD (CV <= 0); negative CV.
  7. A rule (INC_COUNTER on COUNTER[0]) makes CV climb on the FB block and Q
     turns on at PV: the "dual generation" path the App relies on.
  8. Rejections (exception 0x03 / 0x02) and all-or-nothing: bad mode, bad
     retain index (not a VREG_RETAIN tag), duplicate retain index, a counter
     the board does not have, a 2-block request whose 2nd block is bad (the
     1st must NOT be applied), write past the end, write below 0x0B00.
     FC06 to a single config register is accepted.
  9. App-style deploy: 8 Timers + 8 Counters in 3-block chunks (3, 3, 2).
 10. Cleanup: every block "unused", CLEAR_RULES, COUNTER tags back to 0.

Optional:
  --probe-64  Also send a 4-block FC16 (73-byte frame) and report whether the
              board answers. This is the known "FC16 longer than one 64-byte
              USB CDC packet is not answered" issue; it is informational (no
              PASS/FAIL) and runs last because an unanswered frame can leave
              the link needing a moment to recover.

NOT covered yet (later steps): Flash persistence of the FB config, counter
retain across REBOOT, CLEAR_RETAIN / FACTORY_RESET on counters, PVD.

Usage:
  python test_fb.py COM14
  python test_fb.py COM14 --probe-64
"""

import argparse
import sys
import time

from pymodbus.client import ModbusSerialClient
from pymodbus.exceptions import ModbusException

import test_plc as tp        # rule encoding / CRC helpers
import test_sysclear as ts   # Dev wrapper, check(), diag constants
import test_rtc as tr        # upload_rules(), time_rule(), set_time()

REG_RESOURCE = 0x0020
REG_TIMERS = 0x0B00
REG_COUNTERS = 0x0B40
BLOCKS = 8
BLOCK_REGS = 8
RETAIN_NONE = 0xFFFF
Q_BIT = 0x0008
EXC_ADDR, EXC_VALUE = 2, 3

check = ts.check
exc_code = ts.exc_code


# ---------------------------------------------------------------- helpers
def s32(hi, lo):
    v = (hi << 16) | lo
    return v - (1 << 32) if v & 0x80000000 else v


def u32_regs(v):
    v &= 0xFFFFFFFF
    return [v >> 16, v & 0xFFFF]


def timer_block(mode, pt_ms, status=0, et=0, reserved=(0, 0)):
    return [status, mode, *u32_regs(pt_ms), *u32_regs(et), *reserved]


def counter_block(mode, pv, retain=RETAIN_NONE, status=0, cv=0, reserved=0):
    return [status, mode, *u32_regs(pv), *u32_regs(cv), retain, reserved]


UNUSED_TIMER = timer_block(0, 0)
UNUSED_COUNTER = counter_block(0, 0)


def write_blocks(d, base, blocks):
    """Write whole records the way the App's Deploy does: at most 3 blocks per
    FC16 (57-byte frame, under the 64-byte USB CDC packet). Returns the first
    failing response, or None."""
    for i in range(0, len(blocks), 3):
        chunk = [r for blk in blocks[i:i + 3] for r in blk]
        resp = d.fc16(base + i * BLOCK_REGS, chunk)
        if resp.isError():
            return resp
    return None


def read_timer(d, i):
    return d.read(REG_TIMERS + i * BLOCK_REGS, BLOCK_REGS)


def read_counter(d, i):
    return d.read(REG_COUNTERS + i * BLOCK_REGS, BLOCK_REGS)


def reset_all_blocks(d):
    r1 = write_blocks(d, REG_TIMERS, [UNUSED_TIMER] * BLOCKS)
    r2 = write_blocks(d, REG_COUNTERS, [UNUSED_COUNTER] * BLOCKS)
    return r1 is None and r2 is None


def enter_diag(d):
    d.fc06(ts.REG_DIAG, ts.CMD_ENTER)
    time.sleep(0.1)
    return d.diag_state() == ts.STATE_CONTROL


def exit_diag(d):
    d.fc06(ts.REG_DIAG, ts.CMD_EXIT)
    time.sleep(0.2)


def diag_set_counter(d, tag, value):
    """Force a COUNTER tag while in DIAG; keeps the lease alive first."""
    d.fc06(ts.REG_DIAG, ts.CMD_HEARTBEAT)
    return d.write_tag(tag, value)


# -------------------------------------------------------------------- run
def run(d, args):
    ts.ensure_running(d)

    # ---- 1. profile + resources -------------------------------------------
    print("\n[1] profile and resources")
    r = d.read(REG_RESOURCE, 10)
    wire, di, do, ai, vflag, vreg, retain_n, cnt_n = r[0], r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    print(f"  wire_profile={wire} DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} "
          f"VREG_RETAIN={retain_n} COUNTER={cnt_n}")
    check(wire == 2, "wire_profile == 2 (FB block is a V2 feature)")
    if wire != 2 or cnt_n < 1 or retain_n < 2:
        print("need Wire Profile 2, >= 1 COUNTER and >= 2 VREG_RETAIN tags")
        sys.exit(2)
    retain0 = di + do + ai + vflag + vreg           # first VREG_RETAIN tag
    cnt0 = retain0 + retain_n                       # first COUNTER tag
    print(f"  VREG_RETAIN0 = tag {retain0}, COUNTER0 = tag {cnt0}")

    # ---- 2. defaults ------------------------------------------------------
    print("\n[2] defaults after writing every block as 'unused'")
    check(reset_all_blocks(d), "all 16 blocks written (3-block chunks)")
    ok = all(read_timer(d, i)[1] == 0 for i in range(BLOCKS)) and \
        all(read_counter(d, i)[1] == 0 and read_counter(d, i)[6] == RETAIN_NONE
            for i in range(BLOCKS))
    check(ok, "every block: mode 0, counters retain_tag_index = 0xFFFF")

    # ---- 3. read sizes ----------------------------------------------------
    print("\n[3] read sizes")
    try:
        check(len(d.read(REG_TIMERS, 64)) == 64, "FC03 0x0B00 x64 answered")
        check(len(d.read(REG_COUNTERS, 64)) == 64, "FC03 0x0B40 x64 answered")
    except RuntimeError as e:
        check(False, f"64-register read failed: {e}")
    try:
        got = d.read(REG_TIMERS, 128)
        print(f"  INFO: 128 registers in ONE FC03 was answered ({len(got)} regs). "
              "Modbus allows at most 125, so this is not guaranteed on other masters.")
    except Exception as e:
        print(f"  INFO: 128 registers in ONE FC03 rejected as expected ({e}). "
              "The App reads 0x0B00..0x0B7F in one request: it must split it "
              "(e.g. 64 + 64).")

    # ---- 4. timer whole-block write ----------------------------------------
    print("\n[4] Timer whole-block write")
    resp = d.fc16(REG_TIMERS, timer_block(1, 70000, status=0x1234, et=0x00070008,
                                          reserved=(0x7777, 0x8888)))
    check(not resp.isError(), "FC16 whole Timer 0 block accepted (junk in status/ET/reserved)")
    t = read_timer(d, 0)
    check(t[1] == 1, f"mode = TON (got {t[1]})")
    check(t[2:4] == [1, 0x1170], f"PT 70000 ms = 0x00011170, High Word first (got {t[2:4]})")
    check(t[0] == 0 and t[4:6] == [0, 0] and t[6:8] == [0, 0],
          "status / ET / reserved ignored, read 0")
    for mode, name in ((2, "TOF"), (3, "TP")):
        d.fc16(REG_TIMERS + 1 * BLOCK_REGS, timer_block(mode, 5000))
        check(read_timer(d, 1)[1] == mode, f"Timer 1 mode {name} stored")

    # ---- 5. counter whole-block write --------------------------------------
    print("\n[5] Counter whole-block write")
    resp = d.fc16(REG_COUNTERS, counter_block(1, 10, retain0, status=0x00FF, cv=999, reserved=5))
    check(not resp.isError(), "FC16 whole Counter 0 block accepted (junk in status/CV/reserved)")
    c = read_counter(d, 0)
    check(c[1] == 1 and c[2:4] == [0, 10], f"CTU, PV=10 (got mode={c[1]} pv={c[2:4]})")
    check(c[6] == retain0, f"retain_tag_index = {retain0} (got {c[6]})")
    check(c[7] == 0, "reserved reads 0")
    check(s32(c[4], c[5]) != 999, "CV is NOT taken from the write (it is the COUNTER tag)")
    if cnt_n >= 2:
        d.fc16(REG_COUNTERS + BLOCK_REGS, counter_block(2, -3, retain0 + 1))
        c1 = read_counter(d, 1)
        check(c1[1] == 2 and s32(c1[2], c1[3]) == -3, f"CTD, PV=-3 read back (got {s32(c1[2], c1[3])})")

    # ---- 6. CV <-> COUNTER tag, Q ------------------------------------------
    print("\n[6] CV follows the COUNTER tag (DIAG), Q for CTU / CTD")
    if not check(enter_diag(d), "ENTER_DIAG -> DIAG_CONTROL"):
        return
    diag_set_counter(d, cnt0, 4)
    c = read_counter(d, 0)
    check(s32(c[4], c[5]) == 4 and not c[0] & Q_BIT, f"CTU PV=10, tag=4 -> CV=4, Q=0 (got CV={s32(c[4], c[5])} status=0x{c[0]:04X})")
    diag_set_counter(d, cnt0, 10)
    c = read_counter(d, 0)
    check(s32(c[4], c[5]) == 10 and c[0] & Q_BIT, "tag=10 -> CV=10, Q=1 (CV >= PV)")
    diag_set_counter(d, cnt0, -7)
    c = read_counter(d, 0)
    check(s32(c[4], c[5]) == -7 and not c[0] & Q_BIT, "tag=-7 -> CV=-7 (negative), Q=0")
    if cnt_n >= 2:
        diag_set_counter(d, cnt0 + 1, 2)
        c1 = read_counter(d, 1)
        check(not c1[0] & Q_BIT, "CTD: tag=2 -> Q=0")
        diag_set_counter(d, cnt0 + 1, 0)
        c1 = read_counter(d, 1)
        check(c1[0] & Q_BIT, "CTD: tag=0 -> Q=1 (CV <= 0)")
        diag_set_counter(d, cnt0 + 1, 0)
    diag_set_counter(d, cnt0, 0)
    exit_diag(d)

    # ---- 7. rule drives the counter ----------------------------------------
    print("\n[7] rule INC_COUNTER on COUNTER[0] -> CV climbs on the FB block")
    d.fc16(REG_COUNTERS, counter_block(1, 50, RETAIN_NONE))
    # All-day Time Window rule (level): +1 every scan once the clock is set.
    tr.set_time(d, tr.local_epoch(2026, 1, 15, 12, 0, 0, tr.TZ_VN))
    rule = tr.time_rule(0, 0, 2359, cnt0)
    check(tr.upload_rules(d, [rule]), "INC_COUNTER rule committed")
    time.sleep(0.3)
    c = read_counter(d, 0)
    cv_a = s32(c[4], c[5])
    time.sleep(0.5)
    c = read_counter(d, 0)
    cv_b = s32(c[4], c[5])
    print(f"  CV {cv_a} -> {cv_b}")
    check(cv_b > cv_a, "CV rises while the rule runs")
    t0 = time.time()
    while time.time() - t0 < 6.0:
        c = read_counter(d, 0)
        if c[0] & Q_BIT:
            break
        time.sleep(0.2)
    check(c[0] & Q_BIT and s32(c[4], c[5]) >= 50, f"Q turns on once CV >= PV=50 (CV={s32(c[4], c[5])})")
    d.run_syscmd(ts.SYS_CLEAR_RULES)
    time.sleep(0.3)
    a = s32(*read_counter(d, 0)[4:6])
    time.sleep(0.4)
    b = s32(*read_counter(d, 0)[4:6])
    check(a == b, "CV stops after CLEAR_RULES")

    # ---- 8. rejections -------------------------------------------------------
    print("\n[8] rejections and all-or-nothing")
    d.fc16(REG_COUNTERS, counter_block(1, 10, retain0))
    d.fc16(REG_TIMERS, timer_block(1, 1000))
    base_t = read_timer(d, 0)
    base_c = read_counter(d, 0)

    check(exc_code(d.fc16(REG_TIMERS, timer_block(9, 1000))) == EXC_VALUE, "Timer mode 9 -> 0x03")
    check(read_timer(d, 0) == base_t, "  Timer 0 unchanged")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(3, 10))) == EXC_VALUE, "Counter mode 3 -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(1, 10, 0))) == EXC_VALUE,
          "retain index 0 (a DI, not a VREG_RETAIN tag) -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(1, 10, 0x0100))) == EXC_VALUE,
          "retain index out of tag range -> 0x03")
    check(read_counter(d, 0) == base_c, "  Counter 0 unchanged after the rejections")
    if cnt_n >= 2:
        check(exc_code(d.fc16(REG_COUNTERS + BLOCK_REGS, counter_block(1, 10, retain0))) == EXC_VALUE,
              "retain index already used by Counter 0 -> 0x03")
    if cnt_n < BLOCKS:
        check(exc_code(d.fc16(REG_COUNTERS + cnt_n * BLOCK_REGS, counter_block(1, 10))) == EXC_VALUE,
              f"Counter {cnt_n} (board has only {cnt_n}) with mode 1 -> 0x03")
        check(not d.fc16(REG_COUNTERS + cnt_n * BLOCK_REGS, UNUSED_COUNTER).isError(),
              f"Counter {cnt_n} with mode 0 is accepted")

    # 2 blocks in one request, the 2nd bad: the 1st must not be applied.
    two = timer_block(2, 4242) + timer_block(9, 1)
    check(exc_code(d.fc16(REG_TIMERS, two)) == EXC_VALUE, "2-block write, 2nd bad -> 0x03")
    check(read_timer(d, 0) == base_t, "  Timer 0 NOT applied (all-or-nothing)")

    check(exc_code(d.fc16(REG_COUNTERS + 7 * BLOCK_REGS + 7, [0, 0])) == EXC_ADDR,
          "write past 0x0B7F -> 0x02")
    check(exc_code(d.fc16(REG_TIMERS - 1, [0, 0])) == EXC_ADDR,
          "write starting below 0x0B00 -> 0x02")
    check(not d.fc06(REG_TIMERS + 1, 2).isError() and read_timer(d, 0)[1] == 2,
          "FC06 to Timer 0 mode accepted (single config register)")
    check(exc_code(d.fc06(REG_TIMERS + 1, 9)) == EXC_VALUE, "FC06 mode 9 -> 0x03")

    # ---- 9. App-style deploy ------------------------------------------------
    print("\n[9] App-style deploy: 8 Timers + 8 Counters, 3-block chunks (3, 3, 2)")
    timers = [timer_block(1 + i % 3, 1000 * (i + 1)) for i in range(BLOCKS)]
    counters = []
    for i in range(BLOCKS):
        if i < cnt_n:
            counters.append(counter_block(1 + i % 2, 5 + i, retain0 + i if i < retain_n else RETAIN_NONE))
        else:
            counters.append(UNUSED_COUNTER)
    check(write_blocks(d, REG_TIMERS, timers) is None, "8 Timers written in 3 FC16 frames")
    check(write_blocks(d, REG_COUNTERS, counters) is None, "8 Counters written in 3 FC16 frames")
    ok_t = all(read_timer(d, i)[1:4] == timers[i][1:4] for i in range(BLOCKS))
    ok_c = all(read_counter(d, i)[1:4] == counters[i][1:4] and read_counter(d, i)[6] == counters[i][6]
               for i in range(BLOCKS))
    check(ok_t, "all 8 Timers read back (mode, PT)")
    check(ok_c, "all 8 Counters read back (mode, PV, retain)")

    # ---- optional: 4-block frame ---------------------------------------------
    if args.probe_64:
        print("\n[P] probe: 4 blocks in one FC16 (73-byte frame)")
        answered = False
        try:
            resp = d.fc16(REG_TIMERS, [r for blk in timers[:4] for r in blk])
            answered = not resp.isError()
        except ModbusException as e:
            # pymodbus raises (after its retries) when nothing comes back.
            print(f"  no response: {e}")
        if answered:
            print("  INFO: answered. The frame limit no longer bites at 4 blocks.")
        else:
            print("  INFO: NOT answered / error. Known FC16 > 64-byte issue; "
                  "the App stays at 3 blocks per request.")
            time.sleep(1.0)
            try:
                d.read(REG_RESOURCE, 1)
                recovered = True
            except Exception:
                recovered = False
            check(recovered, "link still answers normal requests after the unanswered frame")

    # ---- 10. cleanup ---------------------------------------------------------
    print("\n[10] cleanup")
    try:
        check(reset_all_blocks(d), "every FB block back to 'unused'")
        st, err, _ = d.run_syscmd(ts.SYS_CLEAR_RULES)
        check(st == ts.ST_DONE and err == ts.ERR_NONE, f"CLEAR_RULES done (st={st} err={err})")
        if enter_diag(d):
            for i in range(cnt_n):
                diag_set_counter(d, cnt0 + i, 0)
            exit_diag(d)
        check(d.diag_state() == ts.STATE_RUNNING, "back to ENGINE_RUNNING")
        tr.set_time(d, int(time.time()), tr.TZ_VN)
    except RuntimeError as e:
        check(False, f"cleanup failed: {e}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--probe-64", action="store_true",
                   help="also send a 4-block FC16 and report whether it is answered")
    args = p.parse_args()

    client = ModbusSerialClient(port=args.port, baudrate=args.baudrate, parity="N",
                                stopbits=1, bytesize=8, timeout=1.0)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    try:
        run(ts.Dev(client, args.unit), args)
    finally:
        try:
            client.close()
        except Exception:
            pass
    fails = ts._failures
    print("\nALL PASS" if fails == 0 else f"\n{fails} CHECK(S) FAILED")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
#!/usr/bin/env python3
"""
test_fb.py - Verify the Function Block block (0x0B00..0x0B7F, Wire Profile
V2.0, plan steps 8a + 8b) over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial   (same as the other test scripts)
Put this file next to test_plc.py, test_sysclear.py and test_rtc.py (it
reuses their helpers). Board-agnostic: tag indices come from
DeviceResourceInfo (0x0020), dense layout (DI, DO, AI, VFLAG, VREG,
VREG_RETAIN, COUNTER) like plc_tag.c.

WARNING: this test uploads rules (WRITES Flash, replaces the rule table and,
since step 8b, the FB config stored with it), forces CV tags (VREG/VFLAG/COUNTER)
through DIAG, and finishes with CLEAR_RULES (FB config cleared too). Rules already on
the board are lost. Do not run it on a board whose rules you want to keep.

Step 8b semantics (services/plc_fb/plc_fb.h, "Staging"):
  * FC16/FC06 to 0x0B00..0x0B7F go to a DRAFT. Reads return the RUNNING
    config, so right after a write the old values are still read back.
  * Each block the Host writes (any register of it) is flagged. At a
    successful rule COMMIT (0xA000 = 0xA5A5) flagged blocks take their draft,
    every other block becomes unused, and the result is saved to Flash in the
    same record as the rules.
  * A COMMIT that fails (CRC mismatch) drops the draft. CLEAR_RULES /
    FACTORY_RESET clear the FB config and the draft. A wrong magic written to
    0xA000 is not a COMMIT and keeps the draft.
  * The Counter +6 register is the tag that holds the counter's CV (the
    App's "Storage Register (CV)"): any VFLAG / VREG / VREG_RETAIN / COUNTER
    tag, 0xFFFF = none. CV and Q read from THAT tag; it is not tied to the
    Counter's own index. Duplicates are checked only between blocks in the draft.
So every check of "the config I wrote" is: write FB, COMMIT (deploy), read.

Checks, in order:
  1. Profile V2, resource counts, tag bases.
  2. Defaults: after a Deploy with no FB written every block reads mode 0.
  3. Read sizes (64 + 64 works; one 128-register read is reported).
  4. Timer whole-block write: invisible before COMMIT, read back after,
     32-bit PT High Word first, junk in firmware-owned fields ignored.
  5. Counter whole-block write: same, incl. negative PV and CV tag of each
     allowed kind (VREG, VFLAG, COUNTER).
  6. CV follows the CV tag written through DIAG; Q for CTU and CTD; a
     Counter with no CV tag reads CV=0, Q=0.
  7. A rule (INC_COUNTER on a VREG_RETAIN CV tag) makes CV climb on the FB
     block; CLEAR_RETAIN brings the count back to 0.
  8. Rejections (0x03 / 0x02), all-or-nothing, FC06 keeps the other fields,
     a block the Deploy did not write becomes unused, duplicate CV tag only
     inside the draft, DI/DO/AI tags refused as CV tag.
  9. App-style deploy: 8 Timers + 8 Counters in 3-block chunks (3, 3, 2).
 10. A failed COMMIT drops the draft: the next Deploy starts clean.
 11. CLEAR_RULES / FACTORY_RESET clear FB config and the draft.
 12. (--reboot) FB config survives REBOOT with the rules, and is gone after
     CLEAR_RULES + REBOOT.
 13. Cleanup.

Optional:
  --reboot    persistence across a reset (the real proof the FB config left
              Flash). The COM port drops during reboot; use --port-after if
              Windows renumbers it.
  --probe-64  also send a 4-block FC16 (73-byte frame) and report whether the
              board answers (informational, runs last).

Usage:
  python test_fb.py COM14
  python test_fb.py COM14 --reboot
  python test_fb.py COM14 --reboot --probe-64
"""
import argparse
import sys
import time

from pymodbus.client import ModbusSerialClient
from pymodbus.exceptions import ModbusException

import test_plc as tp        # rule encoding / CRC helpers
import test_sysclear as ts   # Dev wrapper, check(), diag constants, reboot
import test_rtc as tr        # upload_rules(), time_rule(), set_time()

REG_RESOURCE = 0x0020
REG_TIMERS = 0x0B00
REG_COUNTERS = 0x0B40
REG_STATUS = 0x9000
REG_COMMIT = 0xA000
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
    FC16 (57-byte frame). Goes to the DRAFT. Returns the first failing
    response, or None."""
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


class Ctx:
    """Tag indices that depend on the board (set in run())."""
    noop_tag = 0


def noop_rule():
    """A rule that does nothing visible (increments a VREG at exactly 00:00)."""
    return tr.time_rule(1, 0, 0, Ctx.noop_tag)       # compare_op 1 = EQ, Lo == Hi: minute mark


def commit(d, rules=None):
    """Deploy step that makes the draft FB config live: upload rules + COMMIT."""
    return tr.upload_rules(d, rules if rules is not None else [noop_rule()])


def status(d):
    return d.read(REG_STATUS, 1)[0]


def deploy_fb(d, timers=None, counters=None, rules=None):
    """Write FB blocks (draft) then COMMIT, like the App. Returns True if OK."""
    if timers is not None:
        if write_blocks(d, REG_TIMERS, timers) is not None:
            return False
    if counters is not None:
        if write_blocks(d, REG_COUNTERS, counters) is not None:
            return False
    return commit(d, rules)


def enter_diag(d):
    d.fc06(ts.REG_DIAG, ts.CMD_ENTER)
    time.sleep(0.1)
    return d.diag_state() == ts.STATE_CONTROL


def exit_diag(d):
    d.fc06(ts.REG_DIAG, ts.CMD_EXIT)
    time.sleep(0.2)


def diag_set_counter(d, tag, value):
    d.fc06(ts.REG_DIAG, ts.CMD_HEARTBEAT)
    return d.write_tag(tag, value)


def all_unused(d):
    return all(read_timer(d, i)[1] == 0 for i in range(BLOCKS)) and \
           all(read_counter(d, i)[1] == 0 and read_counter(d, i)[6] == RETAIN_NONE
               for i in range(BLOCKS))


# -------------------------------------------------------------------- run
def run(d, holder, args):
    ts.ensure_running(d)

    # ---- 1. profile + resources -------------------------------------------
    print("\n[1] profile and resources")
    r = d.read(REG_RESOURCE, 10)
    wire, di, do, ai, vflag, vreg, retain_n, cnt_n = r[0], r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    print(f"  wire_profile={wire} DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} "
          f"VREG_RETAIN={retain_n} COUNTER={cnt_n}")
    check(wire == 2, "wire_profile == 2 (FB block is a V2 feature)")
    if wire != 2 or cnt_n < 1 or retain_n < 3 or vreg < 2 or vflag < 1 or do < 1:
        print("need Wire Profile 2, >= 1 COUNTER, >= 3 VREG_RETAIN, >= 2 VREG, >= 1 VFLAG and >= 1 DO tags")
        sys.exit(2)
    do0 = di
    vflag0 = di + do + ai
    vreg0 = vflag0 + vflag
    vreg1 = vreg0 + 1
    retain0 = vreg0 + vreg
    cnt0 = retain0 + retain_n
    Ctx.noop_tag = vreg0
    print(f"  DO0 = tag {do0}, VFLAG0 = tag {vflag0}, VREG0 = tag {vreg0}, "
          f"VREG_RETAIN0 = tag {retain0}, COUNTER0 = tag {cnt0}")

    # ---- 2. defaults ------------------------------------------------------
    print("\n[2] a Deploy that writes no FB leaves every block unused")
    check(commit(d), "baseline Deploy (1 no-op rule, no FB written)")
    check(all_unused(d), "every block: mode 0, counters cv tag = 0xFFFF")

    # ---- 3. read sizes ----------------------------------------------------
    print("\n[3] read sizes")
    try:
        check(len(d.read(REG_TIMERS, 64)) == 64, "FC03 0x0B00 x64 answered")
        check(len(d.read(REG_COUNTERS, 64)) == 64, "FC03 0x0B40 x64 answered")
    except RuntimeError as e:
        check(False, f"64-register read failed: {e}")
    try:
        got = d.read(REG_TIMERS, 128)
        print(f"  INFO: 128 registers in ONE FC03 answered ({len(got)} regs); Modbus allows at most 125.")
    except Exception as e:
        print(f"  INFO: 128 registers in ONE FC03 rejected as expected ({e}). "
              "The App must split it (e.g. 64 + 64).")

    # ---- 4. timer whole-block: draft -> COMMIT -> read ---------------------
    print("\n[4] Timer whole-block write (draft, then COMMIT)")
    resp = d.fc16(REG_TIMERS, timer_block(1, 70000, status=0x1234, et=0x00070008,
                                          reserved=(0x7777, 0x8888)))
    check(not resp.isError(), "FC16 whole Timer 0 block accepted (junk in status/ET/reserved)")
    check(read_timer(d, 0)[1] == 0, "before COMMIT: Timer 0 still reads unused (draft is invisible)")
    d.fc16(REG_TIMERS + BLOCK_REGS, timer_block(2, 5000))
    d.fc16(REG_TIMERS + 2 * BLOCK_REGS, timer_block(3, 5000))
    check(commit(d), "COMMIT")
    t = read_timer(d, 0)
    check(t[1] == 1, f"mode = TON (got {t[1]})")
    check(t[2:4] == [1, 0x1170], f"PT 70000 ms = 0x00011170, High Word first (got {t[2:4]})")
    check(t[0] == 0 and t[4:6] == [0, 0] and t[6:8] == [0, 0], "status / ET / reserved ignored, read 0")
    check(read_timer(d, 1)[1] == 2 and read_timer(d, 2)[1] == 3, "Timer 1 TOF, Timer 2 TP stored")
    check(read_timer(d, 3)[1] == 0, "Timer 3 (not written) is unused")

    # ---- 5. counter whole-block -------------------------------------------
    print("\n[5] Counter whole-block write (draft, then COMMIT); CV tag of any kind")
    d.fc16(REG_COUNTERS, counter_block(1, 10, vreg1, status=0x00FF, cv=999, reserved=5))
    d.fc16(REG_COUNTERS + BLOCK_REGS, counter_block(2, -3, vflag0))
    d.fc16(REG_COUNTERS + 2 * BLOCK_REGS, counter_block(1, 5, cnt0))
    d.fc16(REG_COUNTERS + 3 * BLOCK_REGS, counter_block(1, 10, RETAIN_NONE))
    check(read_counter(d, 0)[1] == 0, "before COMMIT: Counter 0 still reads unused")
    check(commit(d), "COMMIT")
    check(read_timer(d, 0)[1] == 0, "Timer 0 (not written in this Deploy) is now unused")
    c = read_counter(d, 0)
    check(c[1] == 1 and c[2:4] == [0, 10], f"CTU, PV=10 (got mode={c[1]} pv={c[2:4]})")
    check(c[6] == vreg1, f"CV tag = VREG tag {vreg1} accepted and stored (got {c[6]})")
    check(c[7] == 0, "reserved reads 0")
    check(s32(c[4], c[5]) != 999, "CV is NOT taken from the write (it is read from the CV tag)")
    c1 = read_counter(d, 1)
    check(c1[1] == 2 and s32(c1[2], c1[3]) == -3, f"CTD, PV=-3 read back (got {s32(c1[2], c1[3])})")
    check(c1[6] == vflag0, f"CV tag = VFLAG tag {vflag0} accepted (got {c1[6]})")
    check(read_counter(d, 2)[6] == cnt0, f"CV tag = COUNTER tag {cnt0} accepted (got {read_counter(d, 2)[6]})")
    check(read_counter(d, 3)[6] == RETAIN_NONE, "CV tag 0xFFFF accepted")

    # ---- 6. CV <-> CV tag, Q ----------------------------------------------
    print("\n[6] CV follows the CV tag (DIAG), Q for CTU / CTD")
    if not check(enter_diag(d), "ENTER_DIAG -> DIAG_CONTROL"):
        return
    diag_set_counter(d, vreg1, 4)
    c = read_counter(d, 0)
    check(s32(c[4], c[5]) == 4 and not c[0] & Q_BIT, f"Counter 0 (CV tag = VREG, CTU PV=10), tag=4 -> CV=4, Q=0 (status=0x{c[0]:04X})")
    diag_set_counter(d, vreg1, 10)
    c = read_counter(d, 0)
    check(s32(c[4], c[5]) == 10 and c[0] & Q_BIT, "tag=10 -> CV=10, Q=1 (CV >= PV)")
    diag_set_counter(d, vreg1, -7)
    c = read_counter(d, 0)
    check(s32(c[4], c[5]) == -7 and not c[0] & Q_BIT, "tag=-7 -> CV=-7 (negative), Q=0")
    diag_set_counter(d, vflag0, 2)
    check(not read_counter(d, 1)[0] & Q_BIT, "Counter 1 (CV tag = VFLAG, CTD): tag=2 -> Q=0")
    diag_set_counter(d, vflag0, 0)
    check(read_counter(d, 1)[0] & Q_BIT, "CTD: tag=0 -> Q=1 (CV <= 0)")
    diag_set_counter(d, cnt0, 5)
    c = read_counter(d, 2)
    check(s32(c[4], c[5]) == 5 and c[0] & Q_BIT, "Counter 2 (CV tag = COUNTER tag, CTU PV=5): tag=5 -> CV=5, Q=1")
    c3 = read_counter(d, 3)
    check(s32(c3[4], c3[5]) == 0 and not c3[0] & Q_BIT, "Counter 3 (no CV tag): CV=0, Q=0")
    diag_set_counter(d, vreg1, 0)
    diag_set_counter(d, vflag0, 0)
    diag_set_counter(d, cnt0, 0)
    exit_diag(d)

    # ---- 7. rule drives the counter ---------------------------------------
    print("\n[7] rule INC_COUNTER on a VREG_RETAIN CV tag -> CV climbs on the FB block")
    d.fc16(REG_COUNTERS, counter_block(1, 50, retain0))
    tr.set_time(d, tr.local_epoch(2026, 1, 15, 12, 0, 0, tr.TZ_VN))
    rule = tr.time_rule(0, 0, 2359, retain0)    # all-day window: +1 every scan
    check(commit(d, [rule]), "Counter 0 (PV 50, CV tag = VREG_RETAIN0) + INC_COUNTER rule committed in one Deploy")
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
    check(a == b, "CV tag stops changing after CLEAR_RULES")
    check(all_unused(d), "CLEAR_RULES also set every FB block to unused")
    # Counting in a retain tag: CLEAR_RETAIN brings it back to 0 (plc_clear_retain).
    d.fc16(REG_COUNTERS, counter_block(1, 50, retain0))
    check(commit(d), "Counter 0 with CV tag = VREG_RETAIN0 committed again")
    st, err, _ = d.run_syscmd(ts.SYS_CLEAR_RETAIN)
    check(st == ts.ST_DONE and err == ts.ERR_NONE, f"CLEAR_RETAIN done (st={st} err={err})")
    check(s32(*read_counter(d, 0)[4:6]) == 0, "CV (retain tag) reads 0 after CLEAR_RETAIN")

    # ---- 8. rejections ----------------------------------------------------
    print("\n[8] rejections and all-or-nothing (against the draft)")
    check(deploy_fb(d, [timer_block(1, 1000)], [counter_block(1, 10, vreg1)]), "baseline deployed")
    base_t, base_c = read_timer(d, 0), read_counter(d, 0)
    # Re-write the baseline into the draft so Timer 0 / Counter 0 are flagged.
    d.fc16(REG_TIMERS, timer_block(1, 1000))
    d.fc16(REG_COUNTERS, counter_block(1, 10, vreg1))
    check(exc_code(d.fc16(REG_TIMERS, timer_block(9, 1000))) == EXC_VALUE, "Timer mode 9 -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(3, 10))) == EXC_VALUE, "Counter mode 3 -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(1, 10, 0))) == EXC_VALUE,
          "CV tag 0 (a DI) -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(1, 10, do0))) == EXC_VALUE,
          "CV tag = a DO tag -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS, counter_block(1, 10, 0x0100))) == EXC_VALUE,
          "CV tag out of tag range -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS + BLOCK_REGS, counter_block(1, 10, vreg1))) == EXC_VALUE,
          "CV tag already used by Counter 0 IN THE DRAFT -> 0x03")
    two = timer_block(2, 4242) + timer_block(9, 1)
    check(exc_code(d.fc16(REG_TIMERS, two)) == EXC_VALUE, "2-block write, 2nd bad -> 0x03")
    check(exc_code(d.fc16(REG_COUNTERS + 7 * BLOCK_REGS + 7, [0, 0])) == EXC_ADDR, "write past 0x0B7F -> 0x02")
    check(exc_code(d.fc16(REG_TIMERS - 1, [0, 0])) == EXC_ADDR, "write starting below 0x0B00 -> 0x02")
    check(commit(d), "COMMIT after the rejected writes")
    check(read_timer(d, 0) == base_t, "Timer 0 == baseline (2-block write NOT applied: all-or-nothing)")
    check(read_counter(d, 0) == base_c, "Counter 0 == baseline (rejected writes changed nothing)")
    check(read_timer(d, 1)[1] == 0, "Timer 1 unused (the rejected 2-block write did not flag it)")
    # FC06 to a single register: the other fields come from the running config.
    check(not d.fc06(REG_TIMERS + 1, 2).isError(), "FC06 to Timer 0 mode accepted (single config register)")
    check(exc_code(d.fc06(REG_TIMERS + 1, 9)) == EXC_VALUE, "FC06 mode 9 -> 0x03")
    check(commit(d), "COMMIT")
    t = read_timer(d, 0)
    check(t[1] == 2 and t[2:4] == [0, 1000], f"mode changed to 2, PT kept from the running config (got {t[1]}, {t[2:4]})")
    check(read_counter(d, 0)[1] == 0, "Counter 0 (not written in that Deploy) is now unused")
    # Duplicate CV tag is checked only inside the draft, not against the running config.
    check(deploy_fb(d, None, [counter_block(1, 10, retain0)]), "Counter 0 running with CV tag retain0")
    d.fc16(REG_COUNTERS, UNUSED_COUNTER)    # leave the draft empty of Counter 0 ...
    resp = d.fc16(REG_COUNTERS + BLOCK_REGS, counter_block(1, 5, retain0))
    check(not resp.isError(), "Counter 1 may take that CV tag (only the RUNNING Counter 0 uses it, and it will be unused)")
    check(commit(d), "COMMIT")
    check(read_counter(d, 0)[1] == 0 and read_counter(d, 1)[1] == 1 and read_counter(d, 1)[6] == retain0,
          "Counter 0 unused, Counter 1 now holds that CV tag")

    # ---- 9. App-style deploy ----------------------------------------------
    print("\n[9] App-style deploy: 8 Timers + 8 Counters, 3-block chunks (3, 3, 2)")
    timers = [timer_block(1 + i % 3, 1000 * (i + 1)) for i in range(BLOCKS)]
    counters = []
    for i in range(BLOCKS):
        # Counter i counts in VREG_RETAIN[i] (every Counter block works, however
        # many COUNTER tags the board has).
        counters.append(counter_block(1 + i % 2, 5 + i, retain0 + i) if i < retain_n
                        else counter_block(1 + i % 2, 5 + i))
    check(write_blocks(d, REG_TIMERS, timers) is None, "8 Timers written in 3 FC16 frames")
    check(write_blocks(d, REG_COUNTERS, counters) is None, "8 Counters written in 3 FC16 frames")
    check(commit(d), "COMMIT")
    check(all(read_timer(d, i)[1:4] == timers[i][1:4] for i in range(BLOCKS)), "all 8 Timers read back (mode, PT)")
    check(all(read_counter(d, i)[1:4] == counters[i][1:4] and read_counter(d, i)[6] == counters[i][6]
              for i in range(BLOCKS)), "all 8 Counters read back (mode, PV, CV tag)")

    # ---- 10. failed COMMIT drops the draft --------------------------------
    print("\n[10] a failed COMMIT drops the draft; the next Deploy starts clean")
    check(deploy_fb(d, [UNUSED_TIMER] * BLOCKS, [counter_block(1, 10, retain0)] + [UNUSED_COUNTER] * 7),
          "Counter 0 running")
    d.fc16(REG_COUNTERS + BLOCK_REGS, counter_block(2, 99, retain0 + 1))      # draft for the doomed Deploy
    rule = noop_rule()
    d.fc06(tr.REG_RULE_COUNT_STAGED, 1)
    d.fc16(tr.REG_STAGING, rule)
    d.fc06(tr.REG_EXPECTED_CRC, 0x1234)                                       # wrong CRC on purpose
    d.fc06(tr.REG_COMMIT, tr.COMMIT_MAGIC)
    time.sleep(0.3)
    check(status(d) == 4, f"COMMIT with a wrong CRC -> CONFIG_STATUS = ERROR (got {status(d)})")
    check(read_counter(d, 0)[1] == 1 and read_counter(d, 1)[1] == 0, "running FB config untouched by the failed COMMIT")
    check(deploy_fb(d, None, [counter_block(1, 11, retain0)]), "Deploy #2 (Counter 0 only)")
    check(read_counter(d, 1)[1] == 0, "Counter 1 from the failed attempt did NOT leak into Deploy #2")
    check(read_counter(d, 0)[3] == 11, "Counter 0 has the new PV")

    # ---- 11. CLEAR_RULES / FACTORY_RESET ----------------------------------
    print("\n[11] CLEAR_RULES / FACTORY_RESET clear the FB config and the draft")
    check(deploy_fb(d, [timer_block(1, 3000)], [counter_block(1, 4, retain0)]), "FB + 1 rule deployed")
    d.fc16(REG_COUNTERS + 3 * BLOCK_REGS, counter_block(1, 8, retain0 + 2))   # a pending draft
    st, err, _ = d.run_syscmd(ts.SYS_CLEAR_RULES)
    check(st == ts.ST_DONE and err == ts.ERR_NONE, f"CLEAR_RULES done (st={st} err={err})")
    check(all_unused(d) and d.rule_count() == 0, "running FB config cleared, no rules")
    check(commit(d), "COMMIT (no FB written since CLEAR_RULES)")
    check(read_counter(d, 3)[1] == 0, "the draft pending before CLEAR_RULES did not come back")
    check(deploy_fb(d, [timer_block(1, 3000)], [counter_block(1, 4, retain0)]), "FB + 1 rule deployed again")
    st, err, _ = d.run_syscmd(ts.SYS_FACTORY)
    check(st == ts.ST_DONE and err == ts.ERR_NONE, f"FACTORY_RESET done (st={st} err={err})")
    check(all_unused(d) and d.rule_count() == 0, "FACTORY_RESET: FB unused, no rules")

    # ---- 12. reboot --------------------------------------------------------
    if args.reboot:
        print("\n[12] FB config survives REBOOT (saved to Flash with the rule table)")
        timers = [timer_block(1, 2500), timer_block(3, 90000)] + [UNUSED_TIMER] * 6
        counters = [counter_block(1, -4, retain0), counter_block(2, 7, RETAIN_NONE)] + [UNUSED_COUNTER] * 6
        check(deploy_fb(d, timers, counters), "FB + 1 rule deployed")
        before = ([read_timer(d, i) for i in range(BLOCKS)], [read_counter(d, i) for i in range(BLOCKS)])
        nd = ts.reboot_and_reconnect(d, holder, args)
        if not check(nd is not None, "board came back after REBOOT"):
            return
        d = nd
        check(d.rule_count() == 1, "precondition: the rule survived REBOOT (Flash works)")
        after_t = [read_timer(d, i) for i in range(BLOCKS)]
        after_c = [read_counter(d, i) for i in range(BLOCKS)]
        check(after_t[0] == before[0][0] and after_t[1] == before[0][1], "Timer 0/1 config identical after REBOOT")
        check(all(after_c[i][1:4] == before[1][i][1:4] and after_c[i][6] == before[1][i][6]
                  for i in range(BLOCKS)), "all 8 Counter configs identical after REBOOT")
        check(all(after_t[i][1] == 0 for i in range(2, BLOCKS)), "unwritten Timers still unused")
        st, err, _ = d.run_syscmd(ts.SYS_CLEAR_RULES)
        check(st == ts.ST_DONE and err == ts.ERR_NONE, "CLEAR_RULES")
        nd = ts.reboot_and_reconnect(d, holder, args)
        if not check(nd is not None, "board came back after the 2nd REBOOT"):
            return
        d = nd
        check(d.rule_count() == 0 and all_unused(d), "rules AND FB config still gone after REBOOT (cleared in Flash)")

    # ---- optional: 4-block frame ------------------------------------------
    if args.probe_64:
        print("\n[P] probe: 4 blocks in one FC16 (73-byte frame)")
        answered = False
        try:
            resp = d.fc16(REG_TIMERS, [r for blk in timers[:4] for r in blk])
            answered = not resp.isError()
        except ModbusException as e:
            print(f"  no response: {e}")
        print("  INFO: answered." if answered else "  INFO: NOT answered / error (known FC16 > 64-byte issue).")
        time.sleep(1.0)
        try:
            d.read(REG_RESOURCE, 1)
            recovered = True
        except Exception:
            recovered = False
        check(recovered, "link still answers normal requests after the probe")

    # ---- 13. cleanup ------------------------------------------------------
    print("\n[13] cleanup")
    try:
        st, err, _ = d.run_syscmd(ts.SYS_CLEAR_RULES)
        check(st == ts.ST_DONE and err == ts.ERR_NONE, f"CLEAR_RULES done (st={st} err={err})")
        check(all_unused(d), "every FB block unused")
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
    p.add_argument("--reboot", action="store_true",
                   help="also verify the FB config survives a REBOOT")
    p.add_argument("--port-after", default=None,
                   help="COM port to use after a reboot if Windows renumbers it")
    p.add_argument("--probe-64", action="store_true",
                   help="also send a 4-block FC16 and report whether it is answered")
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
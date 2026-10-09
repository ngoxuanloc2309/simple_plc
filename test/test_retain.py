#!/usr/bin/env python3
"""
test_retain.py - Verify VREG_RETAIN persistence (retain log in Flash,
services/plc_retain) over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial   (same as the other test scripts)
Put this file next to test_plc.py, test_sysclear.py and test_rtc.py (it
reuses their helpers). Board-agnostic: tag indices come from
DeviceResourceInfo (0x0020), dense layout (DI, DO, AI, VFLAG, VREG,
VREG_RETAIN, COUNTER) like plc_tag.c.

WARNING: this test WRITES Flash (retain log, and the rule table in step 4),
ERASES all retain values and the rule table on the board (CLEAR_RETAIN /
CLEAR_RULES at the start and at the end), and REBOOTS the board several
times. Do not run it on a board whose rules or retain values you want to
keep. The default 130 commits write 130 records into the 117-record retain
ring, about one pass over it (each sector is rated for ~10,000 erases).

What it covers (all through the normal Wire Profile V2 commands):
  1. Every VREG_RETAIN tag (all 32 on the Zigbee-IO board), with positive and
     negative values, committed through DIAG (COMMIT_RETAIN) is read back
     after a REBOOT. This is the "VREG_RETAIN survives a real REBOOT" check.
  2. N consecutive COMMIT_RETAINs (default 130). The retain log is a ring of
     3 sectors x 39 records = 117 records, so 130 commits cross the first
     sector boundary (record 39) and then wrap the ring (record 118 has to
     erase a sector that already holds old records). The old
     retain_snapshot_write() called the Flash erase BEFORE unlocking the
     Flash, so on a chip that ignores erase while locked, the wrap would
     have failed; this is the check for the fixed code. Each commit must
     report no RETAIN_DIRTY flag and no FLASH_CRC_MISMATCH error. A REBOOT
     in the middle (after commit 45) and one at the end must restore the
     newest values.
  3. CLEAR_RETAIN after the wrap still works and survives a REBOOT (zeros).
  4. Rule-table save and retain commit interleaved (a rule upload now brackets
     its Flash erase+write with retain_flash_op_begin/end): both orders, after
     a REBOOT the rule count AND the retain value are intact.
  5. (--periodic) 5-minute safety net: a rule counts up a VREG_RETAIN tag with
     NO COMMIT_RETAIN; after the snapshot period the value must have been
     written to Flash by retain_service() (REBOOT restores a non-zero value).
     Takes about 6 minutes.

What it does NOT cover:
  * The PVD (low-voltage) emergency write. It needs a supply with hold-up
    energy and a falling VDD; the board does not have that hardware yet, so
    it cannot be exercised over Modbus. Not tested.
  * "A period with no change writes nothing": there is no register that
    shows whether a Flash record was written, so it cannot be observed here.

The COM port drops during each reboot; the script reconnects (up to 20 s).
If Windows gives the board a NEW COM number after re-enumeration, pass
--port-after COMx.

Usage:
  python test_retain.py COM14
  python test_retain.py COM14 --commits 60
  python test_retain.py COM14 --commits 0          (skip step 2)
  python test_retain.py COM14 --periodic           (adds the ~6 minute step 5)
"""
import argparse
import sys
import time

from pymodbus.client import ModbusSerialClient

import test_sysclear as ts   # Dev wrapper, check(), diag constants, reboot
import test_rtc as tr        # set_time(), time_rule(), upload_rules()

check = ts.check
exc_code = ts.exc_code

FLAG_RETAIN_DIRTY = 0x1
ERR_FLASH_CRC_MISMATCH = 3
RING_RECORDS = 117            # 3 sectors x 39 records (splc_flash_define.h)
MID_REBOOT_COMMIT = 45        # past the first sector boundary (record 39)


# ---------------------------------------------------------------- helpers
def diag_block(d):
    """COMMAND, STATE, FLAGS, LEASE_MS, ERROR_CODE of the Diag block (0x0A20)."""
    return d.read(ts.REG_DIAG, 5)


def commit_values(d, items):
    """ENTER_DIAG, write retain tags (RAM draft), COMMIT_RETAIN, EXIT_DIAG.

    Returns True only if every write was accepted AND the commit left no
    RETAIN_DIRTY flag and no FLASH_CRC_MISMATCH error (the firmware latches
    that error when its read-back verify of the new record fails)."""
    ok = True
    d.fc06(ts.REG_DIAG, ts.CMD_ENTER)
    time.sleep(0.05)
    if d.diag_state() != ts.STATE_CONTROL:
        return False
    for n, (idx, value) in enumerate(items.items()):
        if n and n % 8 == 0:
            d.fc06(ts.REG_DIAG, ts.CMD_HEARTBEAT)      # keep the 3 s lease alive
        if exc_code(d.write_tag(idx, value)) is not None:
            ok = False
    d.fc06(ts.REG_DIAG, ts.CMD_COMMIT)
    time.sleep(0.2)
    blk = diag_block(d)
    flags, err = blk[2], blk[4]
    if (flags & FLAG_RETAIN_DIRTY) or err == ERR_FLASH_CRC_MISMATCH:
        ok = False
    d.fc06(ts.REG_DIAG, ts.CMD_EXIT)
    time.sleep(0.15)
    ts.ensure_running(d)
    return ok


def reboot(d, holder, args):
    """REBOOT + reconnect. Returns the new Dev, or None if the board did not
    come back (the caller then stops the run)."""
    nd = ts.reboot_and_reconnect(d, holder, args)
    if nd is not None:
        ts.ensure_running(nd)
    return nd


def pattern(i, k=0):
    """Distinct non-zero value per retain slot; every other slot negative."""
    v = (i + 1) * 7919 + k * 100000
    return -v if i % 2 else v


def tags_equal(d, expected):
    """Return the list of (idx, expected, got) mismatches."""
    bad = []
    for idx, want in expected.items():
        got = d.tag(idx)
        if got != want:
            bad.append((idx, want, got))
    return bad


def clear_all(d):
    st1, e1, _ = d.run_syscmd(ts.SYS_CLEAR_RULES)
    st2, e2, _ = d.run_syscmd(ts.SYS_CLEAR_RETAIN)
    return (st1 == ts.ST_DONE and e1 == ts.ERR_NONE and
            st2 == ts.ST_DONE and e2 == ts.ERR_NONE)


# -------------------------------------------------------------------- run
def run(d, holder, args):
    r = d.read(ts.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    base_do = di
    base_ai = base_do + do
    base_vflag = base_ai + ai
    base_vreg = base_vflag + vflag
    base_retain = base_vreg + vreg
    print(f"di={di} do={do} ai={ai} vflag={vflag} vreg={vreg} retain={retain} counter={counter}")
    print(f"DO0=tag {base_do}, RETAIN0=tag {base_retain} .. {base_retain + retain - 1}")
    if retain < 1:
        print("Board has no VREG_RETAIN group. Aborting.")
        return
    DO0, RT0 = base_do, base_retain
    RTL = base_retain + retain - 1              # last retain tag (== RT0 if only one)
    retain_tags = list(range(base_retain, base_retain + retain))
    ts.ensure_running(d)

    def all_retain_zero():
        return all(d.tag(i) == 0 for i in retain_tags)

    # ------------------------------------------------------------------ 0
    print("\n0. Baseline: CLEAR_RULES + CLEAR_RETAIN")
    check(clear_all(d), "CLEAR_RULES and CLEAR_RETAIN both DONE")
    check(all_retain_zero(), f"all {retain} retain tag(s) read 0")

    # ------------------------------------------------------------------ 1
    print(f"\n1. All {retain} VREG_RETAIN tag(s) survive a REBOOT")
    values = {base_retain + i: pattern(i) for i in range(retain)}
    check(commit_values(d, values), "all retain tags committed through DIAG (no dirty flag, no verify error)")
    bad = tags_equal(d, values)
    check(not bad, "live values read back as written" + (f" (first mismatch {bad[0]})" if bad else ""))
    d = reboot(d, holder, args)
    if d is None:
        return
    bad = tags_equal(d, values)
    check(not bad, f"all {retain} values restored after REBOOT" +
          (f" (first mismatch tag {bad[0][0]}: want {bad[0][1]}, got {bad[0][2]})" if bad else ""))

    # ------------------------------------------------------------------ 2
    n = args.commits
    if n > 0:
        print(f"\n2. {n} consecutive COMMIT_RETAINs (ring = {RING_RECORDS} records)")
        if n <= RING_RECORDS:
            print(f"  (info) {n} <= {RING_RECORDS}: the ring does not wrap, the erase path is NOT exercised;"
                  f" use --commits {RING_RECORDS + 13} or more")
        t0 = time.time()
        bad_commits = []
        for i in range(1, n + 1):
            items = {RT0: 100000 + i}
            items[RTL] = -i if RTL != RT0 else items[RT0]
            if not commit_values(d, items):
                bad_commits.append(i)
            if i % 20 == 0:
                print(f"  ... {i}/{n} commits ({time.time() - t0:.0f} s)")
            if i == MID_REBOOT_COMMIT and n > MID_REBOOT_COMMIT:
                d = reboot(d, holder, args)
                if d is None:
                    return
                want = {RT0: 100000 + i}
                want[RTL] = -i if RTL != RT0 else want[RT0]
                bad = tags_equal(d, want)
                check(not bad, f"after commit {i} (past the first sector boundary) + REBOOT: newest values restored"
                      + (f" (tag {bad[0][0]}: want {bad[0][1]}, got {bad[0][2]})" if bad else ""))
        check(not bad_commits,
              f"all {n} commits clean (no dirty flag, no verify error)"
              + (f"; failed at commit(s) {bad_commits[:10]}" if bad_commits else ""))
        want = {RT0: 100000 + n}
        want[RTL] = -n if RTL != RT0 else want[RT0]
        bad = tags_equal(d, want)
        check(not bad, f"live values after commit {n} are the newest" +
              (f" (tag {bad[0][0]}: want {bad[0][1]}, got {bad[0][2]})" if bad else ""))
        d = reboot(d, holder, args)
        if d is None:
            return
        bad = tags_equal(d, want)
        check(not bad, f"after {n} commits + REBOOT: newest values restored" +
              (f" (tag {bad[0][0]}: want {bad[0][1]}, got {bad[0][2]})" if bad else ""))
    else:
        print("\n2. skipped (--commits 0)")

    # ------------------------------------------------------------------ 3
    print("\n3. CLEAR_RETAIN after the ring has been used")
    st, err, _ = d.run_syscmd(ts.SYS_CLEAR_RETAIN)
    check(st == ts.ST_DONE and err == ts.ERR_NONE,
          f"CLEAR_RETAIN result = {ts.ST_NAMES.get(st, st)}/err {err} (expect DONE/0)")
    check(all_retain_zero(), "all retain tags 0")
    d = reboot(d, holder, args)
    if d is None:
        return
    check(all_retain_zero(), "retain still ZERO after REBOOT (the clear reached Flash)")

    # ------------------------------------------------------------------ 4
    print("\n4. Rule-table save and retain commit interleaved")
    if do < 1:
        print("  skipped: board has no DO tag for the test rule")
    else:
        ts.upload_rule(d, DO0)
        check(d.rule_count() == 1, "RULE_TABLE_INFO == 1 after upload")
        check(commit_values(d, {RT0: 0xBEEF}), "RETAIN0 = 0xBEEF committed after the rule upload")
        d = reboot(d, holder, args)
        if d is None:
            return
        check(d.rule_count() == 1, "rule survives REBOOT")
        check(d.tag(RT0) == 0xBEEF, f"RETAIN0 survives REBOOT (got {d.tag(RT0)})")
        check(commit_values(d, {RT0: 0xCAFE}), "RETAIN0 = 0xCAFE committed before a second rule upload")
        ts.upload_rule(d, DO0)
        check(d.rule_count() == 1, "RULE_TABLE_INFO == 1 after the second upload")
        d = reboot(d, holder, args)
        if d is None:
            return
        check(d.rule_count() == 1, "rule survives REBOOT (second order)")
        check(d.tag(RT0) == 0xCAFE, f"RETAIN0 survives REBOOT (second order, got {d.tag(RT0)})")
        check(clear_all(d), "cleanup: CLEAR_RULES and CLEAR_RETAIN DONE")

    # ------------------------------------------------------------------ 5
    if args.periodic:
        print(f"\n5. Periodic safety net: no COMMIT_RETAIN, wait {args.period_s + 30} s")
        d = reboot(d, holder, args)          # restarts the snapshot period timer
        if d is None:
            return
        check(clear_all(d), "baseline: rules and retain cleared")
        tr.set_time(d, tr.local_epoch(2026, 1, 15, 12, 0, 0, tr.TZ_VN))
        rule = tr.time_rule(0, 0, 2359, RT0)   # all-day window: RETAIN0 += 1 every scan
        check(tr.upload_rules(d, [rule]), "rule that counts RETAIN0 up committed")
        time.sleep(1.0)
        v0 = d.tag(RT0)
        time.sleep(0.5)
        v1 = d.tag(RT0)
        check(v1 > v0, f"RETAIN0 is counting (live {v0} -> {v1})")
        wait = args.period_s + 30
        t0 = time.time()
        while time.time() - t0 < wait:
            left = wait - (time.time() - t0)
            print(f"  ... {left:4.0f} s left (RETAIN0 = {d.tag(RT0)})")
            time.sleep(min(30.0, left))
        d.run_syscmd(ts.SYS_CLEAR_RULES)       # stop counting so the live value is fixed
        time.sleep(0.3)
        live = d.tag(RT0)
        print(f"  live RETAIN0 just before the REBOOT = {live}")
        d = reboot(d, holder, args)
        if d is None:
            return
        got = d.tag(RT0)
        print(f"  RETAIN0 after the REBOOT = {got}")
        check(got > 0, "a non-zero value was restored: retain_service() wrote the snapshot on its own")
        check(got <= live, "restored value is one the live counter had already reached")
        check(clear_all(d), "cleanup: CLEAR_RULES and CLEAR_RETAIN DONE")
    else:
        print("\n5. skipped (add --periodic for the ~6 minute safety-net check)")

    print("\nCleanup: board left with an empty rule table and zero retain.")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--commits", type=int, default=RING_RECORDS + 13,
                   help=f"consecutive COMMIT_RETAINs in step 2 (default {RING_RECORDS + 13}, "
                        f"enough to wrap the ring; 0 skips the step)")
    p.add_argument("--periodic", action="store_true",
                   help="also run step 5, the 5-minute safety-net write (~6 minutes)")
    p.add_argument("--period-s", type=int, default=300,
                   help="RETAIN_SNAPSHOT_PERIOD_MS of the firmware in seconds (default 300)")
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
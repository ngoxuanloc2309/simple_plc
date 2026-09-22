#!/usr/bin/env python3
"""
test_rule_manual_simple.py - Minimal manual tests for guard / dwell / compare
rules on the real board, one rule at a time, with verbose logging.

This is intentionally simpler than test_rule.py's automated suite: it stages
ONE rule, prints exactly what was sent and what the MCU reports back at every
step, and then either watches DI/DO for you to drive by hand, or drives the
condition itself via a Modbus-writable tag (VFLAG0) when the case doesn't
need a physical wire touched.

Reuses the exact wire-format helpers from test_plc.py (encode_rule_registers,
crc16_modbus, rule_registers_to_bytes, read_regs/write_reg/write_regs) rather
than re-deriving them, to avoid re-introducing bugs already fixed once
(see docs/handoff.md section 5.1b).

Usage:
    python test_rule_manual_simple.py COM14 guard
    python test_rule_manual_simple.py COM14 dwell
    python test_rule_manual_simple.py COM14 compare
    python test_rule_manual_simple.py COM14 all
"""

import argparse
import sys
import time

from test_plc import (
    ModbusSerialClient,
    encode_rule_registers, crc16_modbus, rule_registers_to_bytes,
    read_regs, write_reg, write_regs,
    CONFIG_STATUS_NAMES,
    REG_CONFIG_STATUS, REG_CONFIG_ERROR_CODE, REG_RULE_COUNT_STAGED,
    REG_EXPECTED_CRC16, REG_STAGING_RULE_TABLE, REG_COMMIT_COMMAND,
    REG_RUNTIME_TAG_VALUES,
    COMMIT_COMMAND_MAGIC,
    TAG_DI0, TAG_DO0,
    SPLC_TRG_ON_RISE, SPLC_TRG_ON_CHANGE,
    SPLC_OP_NONE,
    SPLC_ACT_SET_TAG,
    GUARD_TAG_NONE,
    s16_pair_to_i32,
)

# Tags not covered by test_plc.py's constants but needed here.
# plc_tag_def.h: VFLAG0..31 start at index 20. Used here as a
# Modbus-writable "virtual DI" so guard/compare cases don't require
# touching a physical wire -- only the dwell case (which needs a real
# 0->nonzero edge on an INPUT tag, since rule_scan() reacts to tag_read(),
# not to a Modbus write on an input tag) needs the operator to drive DI0
# by hand.
TAG_VFLAG0 = 20

SPLC_OP_GT = 3  # plc_rule.h SPLC_CompareOp: GT = 3

STAGE_ONE_RULE_COUNT = 1


# --- shared staging/commit helper, logging every step -----------------------

def stage_and_commit_one_rule(client, unit, label, regs):
    print(f"\n=== Staging rule: {label} ===")
    raw_bytes = rule_registers_to_bytes(regs)
    crc = crc16_modbus(raw_bytes)
    print(f"  rule bytes (hex) = {raw_bytes.hex()}")
    print(f"  CRC-16/MODBUS    = 0x{crc:04X}")

    write_reg(client, unit, REG_RULE_COUNT_STAGED, STAGE_ONE_RULE_COUNT)
    write_regs(client, unit, REG_STAGING_RULE_TABLE, regs)
    write_reg(client, unit, REG_EXPECTED_CRC16, crc)

    status, = read_regs(client, unit, REG_CONFIG_STATUS, 1)
    print(f"  CONFIG_STATUS after staging = {status} ({CONFIG_STATUS_NAMES.get(status, '?')})")

    write_reg(client, unit, REG_COMMIT_COMMAND, COMMIT_COMMAND_MAGIC)

    status, = read_regs(client, unit, REG_CONFIG_STATUS, 1)
    error_code, = read_regs(client, unit, REG_CONFIG_ERROR_CODE, 1)
    print(f"  CONFIG_STATUS after commit  = {status} ({CONFIG_STATUS_NAMES.get(status, '?')})")
    print(f"  CONFIG_ERROR_CODE           = {error_code}")

    if status != 3:  # CONFIG_STATUS_READY
        raise RuntimeError(
            f"Commit did not reach READY (status={status}, error_code={error_code})."
        )
    print("  -> committed OK, rule is now active.")


def read_tag(client, unit, tag_idx):
    addr = REG_RUNTIME_TAG_VALUES + tag_idx * 2
    hi, lo = read_regs(client, unit, addr, 2)
    return s16_pair_to_i32(hi, lo)


def write_tag(client, unit, tag_idx, value):
    """Only valid for Modbus-writable tags (VFLAG/VREG/...), NOT physical
    inputs like DI -- writing DI's RUNTIME_TAG_VALUES register has no
    effect on rule_scan(), which reads the real input, not this register."""
    addr = REG_RUNTIME_TAG_VALUES + tag_idx * 2
    hi = (value >> 16) & 0xFFFF
    lo = value & 0xFFFF
    write_regs(client, unit, addr, [hi, lo])


def watch_tags(client, unit, duration_s, labels_and_tags, note):
    print(f"\n--- Watching {', '.join(l for l, _ in labels_and_tags)} for {duration_s}s ---")
    print(f"  {note}")
    print("  (Ctrl+C to stop early)\n")
    t_end = time.time() + duration_s
    last = None
    try:
        while time.time() < t_end:
            values = [read_tag(client, unit, t) for _, t in labels_and_tags]
            if values != last:
                row = "  ".join(f"{l}={v}" for (l, _), v in zip(labels_and_tags, values))
                print(f"  t={time.time():.1f}  {row}")
                last = values
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass


# --- Case 1: GUARD -----------------------------------------------------------
#
# IF DI0 changes (rise or fall) AND guard (VFLAG0) is open -> TOGGLE DO0
#
# VFLAG0 is Modbus-writable, so this case does NOT require touching a
# physical wire for the guard itself -- only DI0 needs a real edge.
# guard closed (VFLAG0=0) should block DO0 from toggling; guard open
# (VFLAG0=1) should let it through. Uses TRG_ON_CHANGE so either a rising
# or falling edge on DI0 counts, making it easy to trigger repeatedly by
# hand without needing to release and re-press for every rise.

def test_guard(client, unit, watch_s):
    print("\n" + "#" * 70)
    print("# CASE: GUARD -- IF DI0 changes AND VFLAG0 (guard) is open -> TOGGLE DO0")
    print("#" * 70)
    print("  guard_tag = TAG_VFLAG0, NEGATE = 0 (guard must be truthy to pass)")
    print("  Set VFLAG0=0 first (closed) to confirm DO0 does NOT toggle,")
    print("  then set VFLAG0=1 (open) and drive DI0 to confirm it DOES.")

    ACT_TOGGLE_TAG = 1  # plc_rule.h SPLC_ActionType: TOGGLE_TAG = 1

    regs = encode_rule_registers(
        threshold_lo=0, threshold_hi=0,
        for_ms=0,
        action_param=0,  # unused by TOGGLE_TAG
        trigger_tag=TAG_DI0,
        action_tag=TAG_DO0,
        guard_tag=TAG_VFLAG0,
        enabled=1,
        trigger_type=SPLC_TRG_ON_CHANGE,
        compare_op=SPLC_OP_NONE,
        action_type=ACT_TOGGLE_TAG,
    )
    stage_and_commit_one_rule(client, unit, "guard test", regs)

    print("\n  Step 1: closing guard (VFLAG0 = 0)...")
    write_tag(client, unit, TAG_VFLAG0, 0)
    do0_before = read_tag(client, unit, TAG_DO0)
    print(f"  DO0 before = {do0_before}. Now drive DI0 (any edge) and watch --")
    print("  DO0 should NOT change while guard is closed.")
    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("VFLAG0(guard)", TAG_VFLAG0), ("DO0", TAG_DO0)],
               "guard CLOSED -- DO0 must stay constant no matter what DI0 does")

    print("\n  Step 2: opening guard (VFLAG0 = 1)...")
    write_tag(client, unit, TAG_VFLAG0, 1)
    print("  Now drive DI0 again -- DO0 SHOULD toggle this time.")
    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("VFLAG0(guard)", TAG_VFLAG0), ("DO0", TAG_DO0)],
               "guard OPEN -- DO0 should flip each time DI0 changes")


# --- Case 2: DWELL -----------------------------------------------------------
#
# IF DI0 stays high for >= 500ms -> SET DO0 = 1
#
# This directly exercises the dwell bugfix from handoff.md section 0b
# (DWELLING branch previously never armed for non-edge triggers; also
# covers the ordinary edge-trigger dwell path). Needs a REAL edge on DI0 --
# rule_scan() only reacts to tag_read(TAG_DI0) changing, not to a Modbus
# write, since DI0 is a physical input (RUNTIME_TAG_VALUES is read-only
# for it, per test_plc.py's own docstring).

def test_dwell(client, unit, watch_s, for_ms):
    print("\n" + "#" * 70)
    print(f"# CASE: DWELL -- IF DI0 stays high for >= {for_ms}ms -> SET DO0 = 1")
    print("#" * 70)
    print(f"  Hold DI0 high for LESS than {for_ms}ms: DO0 should stay 0.")
    print(f"  Hold DI0 high for MORE than {for_ms}ms: DO0 should become 1.")
    print("  (This is the exact scenario handoff.md 0b's dwell bugfix targets --")
    print("   before the fix, dwell rules never fired at all.)")

    regs = encode_rule_registers(
        threshold_lo=0, threshold_hi=0,
        for_ms=for_ms,
        action_param=1,
        trigger_tag=TAG_DI0,
        action_tag=TAG_DO0,
        guard_tag=GUARD_TAG_NONE,
        enabled=1,
        trigger_type=SPLC_TRG_ON_RISE,
        compare_op=SPLC_OP_NONE,
        action_type=SPLC_ACT_SET_TAG,
    )
    stage_and_commit_one_rule(client, unit, "dwell test", regs)

    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("DO0", TAG_DO0)],
               f"try a SHORT hold (<{for_ms}ms) first, then a LONG hold (>{for_ms}ms)")


# --- Case 3: COMPARE ---------------------------------------------------------
#
# IF VFLAG0 > 50 -> SET DO0 = 1
#
# Uses VFLAG0 (Modbus-writable) as the compared value so the operator can
# drive both sides of the condition from this script without touching any
# wire, and see DO0 react in a single terminal.

def test_compare(client, unit, watch_s, threshold):
    print("\n" + "#" * 70)
    print(f"# CASE: COMPARE -- IF VFLAG0 > {threshold} -> SET DO0 = 1")
    print("#" * 70)
    print(f"  This script will write VFLAG0 to a few values below/above {threshold}")
    print("  itself and print DO0 after each -- no physical wiring needed.")

    regs = encode_rule_registers(
        threshold_lo=threshold, threshold_hi=0,  # threshold_hi unused by GT
        for_ms=0,
        action_param=1,
        trigger_tag=TAG_VFLAG0,
        action_tag=TAG_DO0,
        guard_tag=GUARD_TAG_NONE,
        enabled=1,
        trigger_type=SPLC_TRG_ON_CHANGE,
        compare_op=SPLC_OP_GT,
        action_type=SPLC_ACT_SET_TAG,
    )
    stage_and_commit_one_rule(client, unit, "compare test", regs)

    # SET_TAG only ever writes 1 (action_param) when the rule fires; it
    # never clears DO0 back to 0 by itself. Clear DO0 first via a second
    # quick rule swap would be overkill for a simple manual check -- instead
    # just show the raw firing behaviour and let the user read the log.
    test_values = [threshold - 10, threshold, threshold + 1, threshold + 10, threshold - 5]
    for v in test_values:
        write_tag(client, unit, TAG_VFLAG0, v)
        time.sleep(0.3)  # let a few scan cycles pass
        do0 = read_tag(client, unit, TAG_DO0)
        expect_fire = v > threshold
        print(f"  VFLAG0 = {v:>4}  (expect fire={expect_fire!s:<5})  -> DO0 = {do0}")
    print("\n  Note: SET_TAG only ever writes 1 when the rule fires -- DO0 will")
    print("  latch at 1 after the first value above threshold and stay there.")
    print("  Re-run this test (which re-commits the rule) to reset and re-check.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="Serial port, e.g. COM14 or /dev/ttyACM0")
    parser.add_argument("case", choices=["guard", "dwell", "compare", "all"])
    parser.add_argument("--unit", type=int, default=1)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--watch-seconds", type=int, default=15,
                         help="How long to watch tags per step (default 15s)")
    parser.add_argument("--dwell-ms", type=int, default=500,
                         help="for_ms used by the dwell case (default 500)")
    parser.add_argument("--compare-threshold", type=int, default=50,
                         help="threshold_lo used by the compare case (default 50)")
    args = parser.parse_args()

    client = ModbusSerialClient(
        port=args.port, baudrate=args.baudrate,
        bytesize=8, parity="N", stopbits=1, timeout=1,
    )
    if not client.connect():
        print(f"Failed to open {args.port}")
        sys.exit(1)

    try:
        cases = ["guard", "dwell", "compare"] if args.case == "all" else [args.case]
        for case in cases:
            if case == "guard":
                test_guard(client, args.unit, args.watch_seconds)
            elif case == "dwell":
                test_dwell(client, args.unit, args.watch_seconds, args.dwell_ms)
            elif case == "compare":
                test_compare(client, args.unit, args.watch_seconds, args.compare_threshold)
    finally:
        client.close()


if __name__ == "__main__":
    main()
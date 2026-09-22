#!/usr/bin/env python3
"""
test_rule_manual_simple.py - Minimal manual tests for guard / dwell rules on
the real board, staging TWO rules at once, with verbose logging.

Both cases here use the SAME two-rule pipeline, driven entirely by two
physical inputs (DI0, DI1) -- no Modbus-writable "virtual input" trick is
used, because RUNTIME_TAG_VALUES (0x0900-0x09FF) is Read-Only for EVERY
tag kind, including VFLAG (confirmed on real hardware: firmware logs
"write rejected: address=0x0928 quantity=2 is RO/unmapped" when this
script tried to write VFLAG0 through it). There is no register block in
plc_modbus_cfg.c that lets the App force an arbitrary tag's runtime value
-- the only way to make a VFLAG/VREG change is through a rule's action.

Rule A (the "source"): DI0 rises -> SET VFLAG0 = 1
Rule B (the "guarded"): DI1 rises, guard = VFLAG0 -> SET DO0 = 1

    guard test (for_ms=0 on both):
        press DI1 BEFORE ever pressing DI0 -> DO0 must stay 0 (guard closed,
        VFLAG0 still 0). Then press DI0 once (sets VFLAG0=1), then press
        DI1 -> DO0 must become 1 (guard now open).

    dwell test (for_ms=5000 on both, same two rules): same guard logic,
        but BOTH the DI0->VFLAG0 edge and the DI1->DO0 edge must be HELD
        for >= 5 seconds to fire. A quick tap on either pin should do
        nothing; holding it >= 5s should.

A separate two-rule pipeline covers counting + comparing:

    Rule A: DI0 rises                       -> INC_COUNTER (COUNTER0 += 1)
    Rule B: COUNTER0 changes, COUNTER0 >= 10 -> SET DO0 = 1

    counter test: press DI0 repeatedly (short taps, ON_RISE). COUNTER0
        should climb by 1 per press; DO0 should flip to 1 once COUNTER0
        reaches the threshold (default 10) and stay 1 afterwards. Rule B's
        condition is a COMPARE on its own trigger_tag (COUNTER0), not a
        guard on a separate tag -- see the comment above
        build_counter_pipeline_regs() for why ON_CHANGE (not ON_RISE) is
        required here.

Reuses the exact wire-format helpers from test_plc.py (encode_rule_registers,
crc16_modbus, rule_registers_to_bytes, read_regs/write_reg/write_regs) rather
than re-deriving them, to avoid re-introducing bugs already fixed once
(see docs/handoff.md section 5.1b).

Usage:
    python test_rule_manual_simple.py COM14 guard
    python test_rule_manual_simple.py COM14 dwell
    python test_rule_manual_simple.py COM14 counter
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
    SPLC_TRG_ON_RISE,
    SPLC_OP_NONE,
    SPLC_ACT_SET_TAG,
    GUARD_TAG_NONE,
    s16_pair_to_i32,
)

# Tags/enums not covered by test_plc.py's constants but needed here.
# plc_tag_def.h: DI1 = index 1, VFLAG0 = index 20 (first of 32 VFLAGs,
# 20..51), COUNTER0 = index 116 (first of 8 counters, 116..123). Only
# VFLAG0/COUNTER0 are ever written here -- SET_TAG/INC_COUNTER only ever
# touch the single tag named in action_tag, never the others in that group.
TAG_DI1      = 1
TAG_VFLAG0   = 20
TAG_COUNTER0 = 116

# plc_rule.h enums not in test_plc.py:
SPLC_TRG_ON_CHANGE  = 0   # fires on ANY change, not just 0->1 -- needed for
                          # a counter, which isn't a binary 0/1 signal so
                          # ON_RISE's edge-detect doesn't apply to it.
SPLC_OP_GTE         = 5   # SPLC_CompareOp: NONE=0,EQ=1,NEQ=2,GT=3,LT=4,GTE=5
SPLC_ACT_INC_COUNTER = 2  # SPLC_ActionType: SET_TAG=0,TOGGLE_TAG=1,INC_COUNTER=2;
                          # action_tag += action_param (NOT a fixed +1 -- the
                          # +1 here comes from setting action_param=1 below)

STAGE_TWO_RULE_COUNT = 2


# --- shared staging/commit helper, logging every step -----------------------

def stage_and_commit_rules(client, unit, label, rule_regs_list):
    """rule_regs_list: list of already-encoded 16-register lists, one per
    rule, staged at REG_STAGING_RULE_TABLE + i*16 in the given order."""
    print(f"\n=== Staging {len(rule_regs_list)} rule(s): {label} ===")

    raw_bytes = b"".join(rule_registers_to_bytes(regs) for regs in rule_regs_list)
    crc = crc16_modbus(raw_bytes)
    print(f"  combined rule bytes (hex) = {raw_bytes.hex()}")
    print(f"  CRC-16/MODBUS             = 0x{crc:04X}")

    rule_count = len(rule_regs_list)
    write_reg(client, unit, REG_RULE_COUNT_STAGED, rule_count)
    for i, regs in enumerate(rule_regs_list):
        write_regs(client, unit, REG_STAGING_RULE_TABLE + i * 16, regs)
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
    print("  -> committed OK, both rules are now active.")


def read_tag(client, unit, tag_idx):
    addr = REG_RUNTIME_TAG_VALUES + tag_idx * 2
    hi, lo = read_regs(client, unit, addr, 2)
    return s16_pair_to_i32(hi, lo)


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


def build_guard_pipeline_regs(for_ms):
    """Rule A: DI0 rises -> SET VFLAG0 = 1
       Rule B: DI1 rises, guard = VFLAG0 -> SET DO0 = 1
    Same for_ms applied to both rules (0 for the guard case, 5000 for the
    dwell case) -- for_ms=0 means "no dwell" (fires on the edge itself)."""
    rule_a = encode_rule_registers(
        threshold_lo=0, threshold_hi=0,
        for_ms=for_ms,
        action_param=1,               # SET_TAG writes this into action_tag
        trigger_tag=TAG_DI0,
        action_tag=TAG_VFLAG0,
        guard_tag=GUARD_TAG_NONE,     # no guard on the source rule
        enabled=1,
        trigger_type=SPLC_TRG_ON_RISE,
        compare_op=SPLC_OP_NONE,
        action_type=SPLC_ACT_SET_TAG,
    )
    rule_b = encode_rule_registers(
        threshold_lo=0, threshold_hi=0,
        for_ms=for_ms,
        action_param=1,
        trigger_tag=TAG_DI1,
        action_tag=TAG_DO0,
        guard_tag=TAG_VFLAG0,         # guarded rule: only fires if VFLAG0 is truthy
        enabled=1,
        trigger_type=SPLC_TRG_ON_RISE,
        compare_op=SPLC_OP_NONE,
        action_type=SPLC_ACT_SET_TAG,
    )
    return [rule_a, rule_b]


# --- Case 1: GUARD (for_ms = 0) ----------------------------------------------

def test_guard(client, unit, watch_s):
    print("\n" + "#" * 70)
    print("# CASE: GUARD")
    print("#   Rule A: DI0 rises            -> SET VFLAG0 = 1")
    print("#   Rule B: DI1 rises, guard=VFLAG0 -> SET DO0 = 1")
    print("#" * 70)
    print("  Step 1: press DI1 FIRST, BEFORE ever touching DI0.")
    print("          Expected: DO0 stays 0 (guard closed, VFLAG0 is still 0).")
    print("  Step 2: press DI0 once (this sets VFLAG0=1), then press DI1 again.")
    print("          Expected: DO0 becomes 1 this time (guard now open).")

    regs = build_guard_pipeline_regs(for_ms=0)
    stage_and_commit_rules(client, unit, "guard test (for_ms=0)", regs)

    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("DI1", TAG_DI1), ("VFLAG0(guard)", TAG_VFLAG0), ("DO0", TAG_DO0)],
               "press DI1 now, WITHOUT pressing DI0 first -- DO0 should stay 0")

    print("\n  Now press DI0 once, then press DI1 again --")
    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("DI1", TAG_DI1), ("VFLAG0(guard)", TAG_VFLAG0), ("DO0", TAG_DO0)],
               "after DI0's edge sets VFLAG0=1, DI1's edge should now set DO0=1")


# --- Case 2: DWELL (for_ms = 5000, same pipeline) ----------------------------

def test_dwell(client, unit, watch_s, for_ms):
    print("\n" + "#" * 70)
    print("# CASE: DWELL (same guard pipeline, both rules now require a hold)")
    print(f"#   Rule A: DI0 rises, dwell {for_ms}ms            -> SET VFLAG0 = 1")
    print(f"#   Rule B: DI1 rises, dwell {for_ms}ms, guard=VFLAG0 -> SET DO0 = 1")
    print("#" * 70)
    print(f"  This exercises the dwell bugfix from handoff.md 0b (DWELLING branch")
    print(f"  previously never armed for INTERVAL/TIME_WINDOW triggers; this uses")
    print(f"  ON_RISE, which was already working before that fix, but still needs")
    print(f"  now_ms wired through rule_scan() correctly to time the {for_ms}ms hold).")
    print(f"  Step 1: quickly TAP DI0 (hold < {for_ms}ms) -- VFLAG0 should NOT become 1.")
    print(f"  Step 2: HOLD DI0 for >= {for_ms}ms -- VFLAG0 SHOULD become 1.")
    print(f"  Step 3: quickly TAP DI1 -- DO0 should NOT become 1 (dwell too short).")
    print(f"  Step 4: HOLD DI1 for >= {for_ms}ms -- DO0 SHOULD become 1 (guard is open by now).")

    regs = build_guard_pipeline_regs(for_ms=for_ms)
    stage_and_commit_rules(client, unit, f"dwell test (for_ms={for_ms})", regs)

    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("DI1", TAG_DI1), ("VFLAG0(guard)", TAG_VFLAG0), ("DO0", TAG_DO0)],
               f"try a SHORT tap on DI0 first (<{for_ms}ms), then a LONG hold (>={for_ms}ms)")

    print("\n  Now that VFLAG0 should be 1, try DI1 the same way --")
    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("DI1", TAG_DI1), ("VFLAG0(guard)", TAG_VFLAG0), ("DO0", TAG_DO0)],
               f"try a SHORT tap on DI1 first (<{for_ms}ms), then a LONG hold (>={for_ms}ms)")


# --- Case 3: COUNTER + COMPARE -----------------------------------------------
#
# Rule A: DI0 rises            -> INC_COUNTER (COUNTER0 += 1)
# Rule B: COUNTER0 changes, COUNTER0 >= 10 (compare, NOT guard) -> SET DO0 = 1
#
# Rule B's trigger_tag is COUNTER0 itself (not a physical input), so it
# must use ON_CHANGE, not ON_RISE/ON_FALL -- those only make sense for a
# binary 0/1 signal, and COUNTER0 is a plain increasing integer. ON_CHANGE
# re-wakes the rule every time COUNTER0's value differs from last scan,
# letting compare_ok() re-check ">= 10" at every step.
#
# This is deliberately a COMPARE on trigger_tag itself, not a guard on a
# separate tag -- guard_tag stays GUARD_TAG_NONE here.

def build_counter_pipeline_regs(threshold):
    rule_a = encode_rule_registers(
        threshold_lo=0, threshold_hi=0,
        for_ms=0,
        action_param=1,                # INC_COUNTER: action_tag += 1 each fire
        trigger_tag=TAG_DI0,
        action_tag=TAG_COUNTER0,
        guard_tag=GUARD_TAG_NONE,
        enabled=1,
        trigger_type=SPLC_TRG_ON_RISE,
        compare_op=SPLC_OP_NONE,
        action_type=SPLC_ACT_INC_COUNTER,
    )
    rule_b = encode_rule_registers(
        threshold_lo=threshold, threshold_hi=0,  # compares trigger_tag (COUNTER0) itself
        for_ms=0,
        action_param=1,
        trigger_tag=TAG_COUNTER0,
        action_tag=TAG_DO0,
        guard_tag=GUARD_TAG_NONE,      # NOT a guard case -- compare is on trigger_tag itself
        enabled=1,
        trigger_type=SPLC_TRG_ON_CHANGE,
        compare_op=SPLC_OP_GTE,
        action_type=SPLC_ACT_SET_TAG,
    )
    return [rule_a, rule_b]


def test_counter(client, unit, watch_s, threshold):
    print("\n" + "#" * 70)
    print("# CASE: COUNTER + COMPARE")
    print("#   Rule A: DI0 rises                     -> COUNTER0 += 1")
    print(f"#   Rule B: COUNTER0 changes, COUNTER0 >= {threshold} -> SET DO0 = 1")
    print("#" * 70)
    print(f"  Press DI0 (any short tap counts -- ON_RISE, no dwell) {threshold} times.")
    print(f"  COUNTER0 should go up by 1 each press. DO0 should stay 0 until")
    print(f"  COUNTER0 reaches {threshold}, then become 1 and stay 1 (SET_TAG does")
    print(f"  not clear it back down; the compare stays true for any COUNTER0 >= {threshold}")
    print(f"  too, since nothing here ever decrements the counter).")

    regs = build_counter_pipeline_regs(threshold=threshold)
    stage_and_commit_rules(client, unit, f"counter test (threshold={threshold})", regs)

    watch_tags(client, unit, watch_s,
               [("DI0", TAG_DI0), ("COUNTER0", TAG_COUNTER0), ("DO0", TAG_DO0)],
               f"press DI0 repeatedly -- watch COUNTER0 climb, DO0 flip at {threshold}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="Serial port, e.g. COM14 or /dev/ttyACM0")
    parser.add_argument("case", choices=["guard", "dwell", "counter", "all"])
    parser.add_argument("--unit", type=int, default=1)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--watch-seconds", type=int, default=20,
                         help="How long to watch tags per step (default 20s)")
    parser.add_argument("--dwell-ms", type=int, default=5000,
                         help="for_ms used by the dwell case (default 5000, per user spec)")
    parser.add_argument("--counter-threshold", type=int, default=10,
                         help="COUNTER0 threshold used by the counter case (default 10, per user spec)")
    args = parser.parse_args()

    client = ModbusSerialClient(
        port=args.port, baudrate=args.baudrate,
        bytesize=8, parity="N", stopbits=1, timeout=1,
    )
    if not client.connect():
        print(f"Failed to open {args.port}")
        sys.exit(1)

    try:
        cases = ["guard", "dwell", "counter"] if args.case == "all" else [args.case]
        for case in cases:
            if case == "guard":
                test_guard(client, args.unit, args.watch_seconds)
            elif case == "dwell":
                test_dwell(client, args.unit, args.watch_seconds, args.dwell_ms)
            elif case == "counter":
                test_counter(client, args.unit, args.watch_seconds, args.counter_threshold)
    finally:
        client.close()


if __name__ == "__main__":
    main()
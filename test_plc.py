#!/usr/bin/env python3
"""
test_simple_plc_rule.py - Test SimplePLC's Rule Engine over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial
(tested with pymodbus 3.x; if you have an older 2.x install, the client
 call signatures differ slightly -- see the comments below)

What this does
---------------
1. Connects to the MCU's Modbus RTU server over the USB-CDC virtual COM port.
2. Reads DEVICE_DESCRIPTOR / DEVICE_RESOURCE_INFO / RULE_TABLE_INFO as a
   sanity check that the link + register map are working at all.
3. Stages ONE simple rule with no dwell, no guard, no compare:

       IF DI0 rises  ->  SET DO0 = 1

   using the exact staging/commit protocol from
   plc_modbus_cfg.c (0x9000-0xA001), matching the real wire layout
   (SPLC_RuleRecord, 32 bytes / 16 registers, high-word-first for every
   32-bit field) instead of guessing at it.
4. Commits it (COMMIT_COMMAND = 0xA5A5) and confirms CONFIG_STATUS reads
   back READY (3), not ERROR (4).
5. Reads back ACTIVE_RULE_TABLE (0x0100) to confirm the MCU's committed
   copy round-trips identically to what was staged.
6. Polls RUNTIME_TAG_VALUES for DI0/DO0 in a loop so you can watch DO0
   flip when you drive DI0 high on the real board -- this is the actual
   "does the rule engine work" test, since rule_scan() only reacts to a
   real 0->nonzero transition on tag_read(TAG_DI0), not to a Modbus
   write (DI0 is a physical input, RUNTIME_TAG_VALUES is read-only for it).

Register map / wire format source: services/plc_modbus_cfg/plc_modbus_cfg.c
and core/plc_rule/plc_rule.h, read directly from the repo -- not guessed.

IMPORTANT notes carried over from the architecture doc, so a "test failed"
result here isn't mistaken for a Rule Engine bug:
- s_rule_scan_now_ms in plc_rule.c is currently hardcoded to 0 (never wired
  to a real tick). This script's rule uses for_ms=0 (no dwell) specifically
  to avoid that known limitation -- a rule with for_ms>0 will currently get
  stuck in DWELLING forever and never fire. Don't add dwell to this test
  until that TODO is resolved.
- guard_tag is set to GUARD_TAG_NONE (0x7FFF), the correct "no guard"
  sentinel under the current v1.9 tag layout (NOT 0 -- 0 is TAG_DI0, a real
  tag, see plc_rule.h).
"""

import argparse
import inspect
import struct
import sys
import time

try:
    from pymodbus.client import ModbusSerialClient
except ImportError:
    print("Missing dependency. Install with:  pip install pymodbus pyserial")
    sys.exit(1)


# --- Register map constants (from plc_modbus_cfg.c) -------------------------

REG_DEVICE_DESCRIPTOR      = 0x0000  # 10 registers, RO
REG_RULE_TABLE_INFO        = 0x0010  # 1 register, RO
REG_DEVICE_RESOURCE_INFO   = 0x0020  # 10 registers, RO
REG_ACTIVE_RULE_TABLE      = 0x0100  # 16 regs/rule, RO
REG_DEVICE_HEALTH          = 0x0800  # 10 registers, RO
REG_RUNTIME_TAG_VALUES     = 0x0900  # 2 regs/tag, RO

REG_CONFIG_STATUS          = 0x9000  # RO
REG_CONFIG_ERROR_CODE      = 0x9001  # RO
REG_RULE_COUNT_STAGED      = 0x9002  # RW
REG_EXPECTED_CRC16         = 0x9003  # RW
REG_ACTIVE_RULE_COUNT      = 0x9004  # RO
REG_ACTIVE_RULE_CRC16      = 0x9005  # RO
REG_STAGING_RULE_TABLE     = 0x9010  # 16 regs/rule, RW

REG_SYSTEM_COMMAND         = 0x0A00  # WO
REG_COMMIT_COMMAND         = 0xA000  # WO
REG_ACTIVE_RULE_VERSION    = 0xA001  # RO

COMMIT_COMMAND_MAGIC = 0xA5A5

CONFIG_STATUS_NAMES = {0: "IDLE", 1: "RECEIVING", 2: "VERIFYING", 3: "READY", 4: "ERROR"}

# --- Tag indices (plc_tag_def.h, Remote I/O SKU, v1.9 layout) ---------------

TAG_DI0 = 0
TAG_DO0 = 8

# --- SPLC_RuleRecord enums (plc_rule.h) -------------------------------------

SPLC_TRG_ON_CHANGE   = 0
SPLC_TRG_ON_RISE     = 1
SPLC_TRG_ON_FALL     = 2
SPLC_TRG_TIME_WINDOW = 3
SPLC_TRG_INTERVAL    = 4

SPLC_OP_NONE = 0

SPLC_ACT_SET_TAG = 0

GUARD_TAG_NONE = 0x7FFF  # "no guard" sentinel -- NOT 0 (0 is TAG_DI0, a real tag)


# --- CRC-16/MODBUS, matching nmbs_crc_calc() (used to verify the staged ----
#     rule table before COMMIT_COMMAND, exactly like plc_modbus_cfg.c does).

def crc16_modbus(data: bytes) -> int:
    """CRC-16/MODBUS: poly 0xA001 (reflected 0x8005), init 0xFFFF."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


# --- SPLC_RuleRecord <-> 16 x uint16 register encode/decode -----------------
#
# Matches plc_modbus_cfg.c's read_staging_rule_table()/write_staging_rule_table()
# field_off table exactly:
#   0: threshold_lo high word   1: threshold_lo low word
#   2: threshold_hi high word   3: threshold_hi low word
#   4: for_ms high word         5: for_ms low word
#   6: action_param high word   7: action_param low word
#   8: trigger_tag              9: action_tag             10: guard_tag
#   11: (enabled<<8)|trigger_type      12: (compare_op<<8)|action_type
#   13-15: reserved (write 0)

def encode_rule_registers(threshold_lo, threshold_hi, for_ms, action_param,
                           trigger_tag, action_tag, guard_tag,
                           enabled, trigger_type, compare_op, action_type):
    def hi(v32):
        return (v32 >> 16) & 0xFFFF

    def lo(v32):
        return v32 & 0xFFFF

    # Python's & 0xFFFF already gives the correct two's-complement bit
    # pattern for negative int32 values, matching the firmware's
    # (uint32_t)x cast before shifting -- no extra masking needed here.
    tl = threshold_lo & 0xFFFFFFFF
    th = threshold_hi & 0xFFFFFFFF
    ap = action_param & 0xFFFFFFFF

    regs = [
        hi(tl), lo(tl),
        hi(th), lo(th),
        hi(for_ms), lo(for_ms),
        hi(ap), lo(ap),
        trigger_tag & 0xFFFF,
        action_tag & 0xFFFF,
        guard_tag & 0xFFFF,
        ((enabled & 0xFF) << 8) | (trigger_type & 0xFF),
        ((compare_op & 0xFF) << 8) | (action_type & 0xFF),
        0, 0, 0,  # reserved
    ]
    assert len(regs) == 16
    return regs


def rule_registers_to_bytes(regs16):
    """Serialize a rule's 16 registers to its 32-byte WIRE image.

    Per docs/SimplePLC_App_MCU_Structs_v1.9 section 8.4, the CRC-16/MODBUS
    of the Rule Table (EXPECTED_CRC16 / ACTIVE_RULE_CRC16) is taken over the
    serialized table: rule_count x 32 bytes, each 16-bit register high byte
    first, each 32-bit field High Word then Low Word. That is exactly the
    register list concatenated big-endian -- independent of the MCU's
    endianness or struct layout, so any App following the spec computes the
    same value.

    (An earlier version of this script packed the little-endian in-memory
    layout of the C struct instead. That only matched a firmware that hashed
    its RAM image directly, which is chip- and layout-dependent; the firmware
    now hashes the wire image, see rule_table_wire_crc16() in
    plc_modbus_cfg.c.)
    """
    assert len(regs16) == 16
    return b"".join(struct.pack(">H", r & 0xFFFF) for r in regs16)


# --- Modbus helpers ----------------------------------------------------------

def _unit_kwarg(client, unit):
    """Return {<name>: unit} using whichever keyword this pymodbus version
    accepts for the Modbus unit/slave id.

    pymodbus renamed the parameter across releases: older 3.x used `slave=`,
    3.10+ uses `device_id=` (and `slave=` raises TypeError). Detect it from
    the method signature so this script works on both."""
    params = inspect.signature(client.read_holding_registers).parameters
    if "device_id" in params:
        return {"device_id": unit}
    if "slave" in params:
        return {"slave": unit}
    raise RuntimeError("Unsupported pymodbus version: read_holding_registers() "
                       "has neither 'device_id' nor 'slave' parameter")


def read_regs(client, unit, addr, count):
    rr = client.read_holding_registers(address=addr, count=count,
                                       **_unit_kwarg(client, unit))
    if rr.isError():
        raise RuntimeError(f"read_holding_registers(addr=0x{addr:04X}, count={count}) failed: {rr}")
    return rr.registers


def write_regs(client, unit, addr, values):
    rr = client.write_registers(address=addr, values=values,
                                **_unit_kwarg(client, unit))
    if rr.isError():
        raise RuntimeError(f"write_registers(addr=0x{addr:04X}) failed: {rr}")


def write_reg(client, unit, addr, value):
    rr = client.write_register(address=addr, value=value,
                               **_unit_kwarg(client, unit))
    if rr.isError():
        raise RuntimeError(f"write_register(addr=0x{addr:04X}, value=0x{value:04X}) failed: {rr}")


def s16_pair_to_i32(hi, lo):
    return struct.unpack(">i", struct.pack(">HH", hi, lo))[0]


# --- Test steps --------------------------------------------------------------

def print_device_info(client, unit):
    print("\n--- DEVICE_DESCRIPTOR (0x0000) ---")
    regs = read_regs(client, unit, REG_DEVICE_DESCRIPTOR, 10)
    (device_class, device_variant,
     hw_major, hw_minor, hw_patch,
     fw_major, fw_minor, fw_patch,
     protocol_version, rule_format_version) = regs
    print(f"  device_class={device_class} device_variant={device_variant}")
    print(f"  hw_version={hw_major}.{hw_minor}.{hw_patch}  fw_version={fw_major}.{fw_minor}.{fw_patch}")
    print(f"  protocol_version={protocol_version} rule_format_version={rule_format_version}")

    print("--- RULE_TABLE_INFO (0x0010) ---")
    (rule_count,) = read_regs(client, unit, REG_RULE_TABLE_INFO, 1)
    print(f"  active rule_count = {rule_count}")

    print("--- DEVICE_HEALTH (0x0800) ---")
    regs = read_regs(client, unit, REG_DEVICE_HEALTH, 10)
    print(f"  raw = {regs}")


def report_rule_upload_done(client, unit, expected_count, expected_crc):
    """Print a clear "rule upload finished" banner, confirmed by READING BACK
    the MCU's own ACTIVE_RULE_COUNT / ACTIVE_RULE_CRC16 / ACTIVE_RULE_VERSION
    (0x9004 / 0x9005 / 0xA001) -- not just by trusting that CONFIG_STATUS
    said READY. If the MCU's numbers differ from what was sent, say so."""
    active_count, = read_regs(client, unit, REG_ACTIVE_RULE_COUNT, 1)
    active_crc,   = read_regs(client, unit, REG_ACTIVE_RULE_CRC16, 1)
    version,      = read_regs(client, unit, REG_ACTIVE_RULE_VERSION, 1)

    count_ok = (active_count == expected_count)
    crc_ok   = (active_crc == expected_crc)

    print()
    print("  ============================================================")
    if count_ok and crc_ok:
        print("  RULE UPLOAD DONE -- MCU confirms the rule table is active")
    else:
        print("  RULE UPLOAD FINISHED BUT MCU REPORTS A DIFFERENT TABLE")
    print("  ------------------------------------------------------------")
    print(f"  rules sent / active on MCU : {expected_count} / {active_count}"
          f"  {'OK' if count_ok else 'MISMATCH'}")
    print(f"  CRC-16 sent / active on MCU: 0x{expected_crc:04X} / 0x{active_crc:04X}"
          f"  {'OK' if crc_ok else 'MISMATCH'}")
    print(f"  active_rule_version        : {version}")
    print("  ============================================================")

    if not (count_ok and crc_ok):
        raise RuntimeError("MCU's ACTIVE_RULE_COUNT/CRC16 do not match what was sent.")


def stage_and_commit_test_rule(client, unit):
    """Stage: IF DI0 rises -> SET DO0 = 1, no dwell, no guard."""
    print("\n--- Staging test rule: IF DI0 rises -> SET DO0=1 ---")

    regs = encode_rule_registers(
        threshold_lo=0,
        threshold_hi=0,
        for_ms=0,                       # no dwell -- see module docstring on why
        action_param=1,                 # ACT_SET_TAG writes this into action_tag
        trigger_tag=TAG_DI0,
        action_tag=TAG_DO0,
        guard_tag=GUARD_TAG_NONE,       # 0x7FFF, NOT 0 -- 0 is TAG_DI0, a real tag
        enabled=1,
        trigger_type=SPLC_TRG_ON_RISE,
        compare_op=SPLC_OP_NONE,
        action_type=SPLC_ACT_SET_TAG,
    )

    rule_count = 1
    raw_bytes = rule_registers_to_bytes(regs)  # one SPLC_RuleRecord, 32 bytes
    crc = crc16_modbus(raw_bytes)
    print(f"  rule bytes (hex) = {raw_bytes.hex()}")
    print(f"  CRC-16/MODBUS    = 0x{crc:04X}")

    # Step 1: write RULE_COUNT_STAGED (0x9002) -- moves CONFIG_STATUS to RECEIVING
    write_reg(client, unit, REG_RULE_COUNT_STAGED, rule_count)

    # Step 2: write the rule's 16 registers into STAGING_RULE_TABLE (0x9010 + i*16)
    write_regs(client, unit, REG_STAGING_RULE_TABLE + 0 * 16, regs)

    # Step 3: write EXPECTED_CRC16 (0x9003)
    write_reg(client, unit, REG_EXPECTED_CRC16, crc)

    status, = read_regs(client, unit, REG_CONFIG_STATUS, 1)
    print(f"  CONFIG_STATUS after staging = {status} ({CONFIG_STATUS_NAMES.get(status, '?')})")

    # Step 4: commit
    print("  Sending COMMIT_COMMAND (0xA5A5)...")
    write_reg(client, unit, REG_COMMIT_COMMAND, COMMIT_COMMAND_MAGIC)

    status, = read_regs(client, unit, REG_CONFIG_STATUS, 1)
    error_code, = read_regs(client, unit, REG_CONFIG_ERROR_CODE, 1)
    print(f"  CONFIG_STATUS after commit  = {status} ({CONFIG_STATUS_NAMES.get(status, '?')})")
    print(f"  CONFIG_ERROR_CODE           = {error_code}")

    if status != 3:  # CONFIG_STATUS_READY
        raise RuntimeError(
            f"Commit did not reach READY (status={status}, error_code={error_code}). "
            "Check CRC/rule_count and plc_modbus_cfg.c's error codes (plc_error.h)."
        )

    report_rule_upload_done(client, unit, rule_count, crc)
    return raw_bytes


def verify_active_rule_table(client, unit, expected_bytes):
    print("\n--- Verifying ACTIVE_RULE_TABLE (0x0100) round-trip ---")
    regs = read_regs(client, unit, REG_ACTIVE_RULE_TABLE, 16)
    active_bytes = rule_registers_to_bytes(regs)
    print(f"  active rule bytes (hex)   = {active_bytes.hex()}")
    print(f"  staged rule bytes (hex)   = {expected_bytes.hex()}")
    if active_bytes != expected_bytes:
        print("  MISMATCH -- active rule table does not match what was staged!")
        return False
    print("  Match -- ACTIVE_RULE_TABLE correctly reflects the committed rule.")
    return True


def read_tag_value(client, unit, tag_idx):
    addr = REG_RUNTIME_TAG_VALUES + tag_idx * 2
    hi, lo = read_regs(client, unit, addr, 2)
    return s16_pair_to_i32(hi, lo)


def watch_di0_do0(client, unit, duration_s):
    """Poll all 4 DI tags + DO0 and print a line whenever any of them changes.

    Watching only DI0 hides wiring/numbering mix-ups: the schematic numbers
    the input channels IO IN1..IN4 while the firmware/CubeMX names them
    IN0..IN3 (TAG_DI0..TAG_DI3), so a signal on the "first" opto channel
    could land on DI0 or DI1. Printing all four shows which tag really moved.
    """
    print(f"\n--- Watching DI0..DI3 / DO0 for {duration_s}s ---")
    print("  Drive an input high (0 -> nonzero) and watch which DI changes.")
    print("  Rule under test: IF DI0 rises -> DO0 = 1.")
    print("  (Ctrl+C to stop early)\n")
    t_end = time.time() + duration_s
    last = None
    try:
        while time.time() < t_end:
            di = [read_tag_value(client, unit, TAG_DI0 + i) for i in range(4)]
            do0 = read_tag_value(client, unit, TAG_DO0)
            row = (*di, do0)
            if row != last:
                print(f"  t={time.time():.1f}  DI0={di[0]} DI1={di[1]} "
                      f"DI2={di[2]} DI3={di[3]}  DO0={do0}")
                last = row
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="Serial port, e.g. COM5 or /dev/ttyACM0")
    parser.add_argument("--unit", type=int, default=1,
                         help="Modbus unit/slave id. MUST match the firmware's MODBUS_UNIT_ID "
                              "(board_zigbee_io.h, currently 1): nanoMODBUS silently ignores "
                              "requests with any other unit id, and 0 is broadcast (never "
                              "answered). Default 1")
    parser.add_argument("--baudrate", type=int, default=115200,
                         help="Baud rate (USB-CDC virtual COM port -- value is mostly cosmetic, "
                              "but pyserial requires one)")
    parser.add_argument("--skip-rule-test", action="store_true",
                         help="Only print device info, don't stage/commit a rule")
    parser.add_argument("--watch-seconds", type=int, default=30,
                         help="How long to watch DI0/DO0 after committing (default 30s, 0 to skip)")
    args = parser.parse_args()

    client = ModbusSerialClient(
        port=args.port,
        baudrate=args.baudrate,
        bytesize=8,
        parity="N",
        stopbits=1,
        timeout=1,
    )

    if not client.connect():
        print(f"Failed to open {args.port}")
        sys.exit(1)

    try:
        print_device_info(client, args.unit)

        if not args.skip_rule_test:
            raw_bytes = stage_and_commit_test_rule(client, args.unit)
            verify_active_rule_table(client, args.unit, raw_bytes)

            if args.watch_seconds > 0:
                watch_di0_do0(client, args.unit, args.watch_seconds)
    finally:
        client.close()


if __name__ == "__main__":
    main()
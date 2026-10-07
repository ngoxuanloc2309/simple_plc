#!/usr/bin/env python3
"""
test_tags.py - Verify SimplePLC runtime tag writes through 0x0900..0x09FF
(Wire Profile V2, plan steps 3 and 5) over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial   (same as test_plc.py / test_diag.py)

Board-agnostic: tag indices are derived from DeviceResourceInfo (0x0020), laid
out densely in wire group order (DI, DO, AI, VFLAG, VREG, VREG_RETAIN,
COUNTER) exactly like core/plc_tag/plc_tag.c does. Works for the 4DI/4DO
Zigbee-IO board and for a bigger SKU without edits.

Checks, in order:
  1. Outside DIAG_CONTROL every tag write is rejected (exception 0x02).
  2. After ENTER_DIAG: DO0, VFLAG0, VREG0 (negative value), COUNTER0 can be
     written with FC16 and read back with FC03.
  3. Always denied: DI0, the first unpopulated slot, the last (reserved)
     slot.
  4. FC06 on the tag area -> exception 0x03; odd quantity -> 0x03; start on a
     low-word address -> 0x03.
  5. All-or-nothing: a span [last COUNTER, first unpopulated tag] is rejected
     and the COUNTER is NOT modified.
  6. Retain draft (step 4): a VREG_RETAIN write only changes a RAM draft.
     RETAIN_DIRTY is set, EXIT_DIAG is refused (ERR_RETAIN_DIRTY), DISCARD
     drops the draft and the old value is back. No Flash is written.
  7. EXIT_DIAG leaves the written values in place for the Rule Engine.

Optional:
  --commit   Step 4 COMMIT_RETAIN: really writes VREG_RETAIN0 to Flash
             (2 Flash records: the new value, then the original is restored).
  --reboot   Step 5. Writes DO0/VFLAG0 in diag, sends SYSTEM_COMMAND REBOOT
             and checks DO0/VFLAG0 read 0 right after the command (before the
             300 ms reboot delay ends). The board then resets: expect the
             COM port to drop. Watch the DO0 LED/relay: it must switch OFF.
  --pins     Pauses (default 5 s, --pin-seconds N) after DO0=1 so you can look
             at the physical output; heartbeats keep the 3 s lease alive.

Usage:
  python test_tags.py COM14
  python test_tags.py COM14 --pins --reboot
"""

import argparse
import inspect
import sys
import time

from pymodbus.client import ModbusSerialClient

REG_RESOURCE = 0x0020
REG_TAGS = 0x0900
REG_DIAG = 0x0A20
REG_SYSCMD = 0x0A00
CMD_ENTER, CMD_HEARTBEAT, CMD_EXIT, CMD_COMMIT, CMD_DISCARD = 1, 2, 3, 4, 5
STATE_RUNNING, STATE_CONTROL = 1, 2
SYSCMD_REBOOT = 1
MAX_TAGS = 128

_failures = 0
PIN_SECONDS = 5.0


def check(cond, msg):
    global _failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
    if not cond:
        _failures += 1
    return cond


def _unit_kwarg(client, unit):
    params = inspect.signature(client.read_holding_registers).parameters
    if "device_id" in params:
        return {"device_id": unit}
    if "slave" in params:
        return {"slave": unit}
    raise RuntimeError("Unsupported pymodbus version")


class Dev:
    def __init__(self, client, unit):
        self.c, self.kw = client, _unit_kwarg(client, unit)

    def read(self, addr, count):
        rr = self.c.read_holding_registers(address=addr, count=count, **self.kw)
        if rr.isError():
            raise RuntimeError(f"read 0x{addr:04X} x{count} failed: {rr}")
        return rr.registers

    def fc06(self, addr, value):
        return self.c.write_register(address=addr, value=value, **self.kw)

    def fc16(self, addr, values):
        return self.c.write_registers(address=addr, values=values, **self.kw)

    def state(self):
        return self.read(REG_DIAG + 1, 1)[0]

    def tag(self, idx):
        hi, lo = self.read(REG_TAGS + 2 * idx, 2)
        v = (hi << 16) | lo
        return v - (1 << 32) if v & 0x80000000 else v

    def write_tag(self, idx, value):
        v = value & 0xFFFFFFFF
        return self.fc16(REG_TAGS + 2 * idx, [v >> 16, v & 0xFFFF])


def exc_code(resp):
    """Modbus exception code of an error response, or None if it succeeded."""
    if not resp.isError():
        return None
    return getattr(resp, "exception_code", -1)


def run(d, do_pins, do_reboot, do_commit):
    r = d.read(REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    base_do = di
    base_ai = base_do + do
    base_vflag = base_ai + ai
    base_vreg = base_vflag + vflag
    base_retain = base_vreg + vreg
    base_counter = base_retain + retain
    populated = base_counter + counter
    print(f"di={di} do={do} ai={ai} vflag={vflag} vreg={vreg} retain={retain} counter={counter}")
    print(f"DO0=tag {base_do}, VFLAG0=tag {base_vflag}, VREG0=tag {base_vreg}, "
          f"RETAIN0=tag {base_retain}, COUNTER0=tag {base_counter}, first unpopulated=tag {populated}")
    if do < 1 or vflag < 1 or vreg < 2 or retain < 1 or counter < 1:
        print("Board lacks a group this test needs (DO, VFLAG, 2 VREG, RETAIN, COUNTER). Aborting.")
        return

    if d.state() != STATE_RUNNING:
        d.fc06(REG_DIAG, CMD_EXIT)
        time.sleep(0.2)
    DO0, VF0, VR0 = base_do, base_vflag, base_vreg
    VRLAST, RT0, CN0 = base_vreg + vreg - 1, base_retain, base_counter

    print("1. Outside DIAG_CONTROL")
    e = exc_code(d.write_tag(DO0, 1))
    check(e == 2, f"FC16 to DO0 rejected with 0x02 (got {e})")
    check(d.tag(DO0) == 0, "DO0 still 0")

    print("2. ENTER_DIAG, then write")
    d.fc06(REG_DIAG, CMD_ENTER)
    t0 = time.time()
    while d.state() != STATE_CONTROL and time.time() - t0 < 1.0:
        time.sleep(0.02)
    if not check(d.state() == STATE_CONTROL, "reached DIAG_CONTROL"):
        return
    check(exc_code(d.write_tag(DO0, 1)) is None and d.tag(DO0) == 1, "DO0 = 1, read back 1")
    if do_pins:
        # Must keep the lease alive: a plain input() easily outlasts 3000 ms.
        print(f"   >>> DO0 should be ON now. Look at the output ({PIN_SECONDS:.0f} s, heartbeats running) ...")
        t_end = time.time() + PIN_SECONDS
        while time.time() < t_end:
            d.fc16(REG_DIAG, [CMD_HEARTBEAT])
            time.sleep(1.0)
    check(exc_code(d.write_tag(VF0, 1)) is None and d.tag(VF0) == 1, "VFLAG0 = 1")
    check(exc_code(d.write_tag(VR0, -123456)) is None and d.tag(VR0) == -123456,
          "VREG0 = -123456 (negative, High Word first)")
    check(exc_code(d.write_tag(CN0, 0x7FFFFFFF)) is None and d.tag(CN0) == 0x7FFFFFFF,
          "COUNTER0 = INT32_MAX")
    d.fc16(REG_DIAG, [CMD_HEARTBEAT])

    print("3. Always denied")
    before = d.tag(0)
    check(exc_code(d.write_tag(0, 1)) == 2 and d.tag(0) == before, "DI0 -> 0x02, value unchanged")
    if populated < MAX_TAGS:
        check(exc_code(d.write_tag(populated, 1)) == 2, f"tag {populated} (unpopulated) -> 0x02")
    check(exc_code(d.write_tag(MAX_TAGS - 1, 1)) == 2, "tag 127 (reserved) -> 0x02")

    print("4. 0x03 cases")
    e = exc_code(d.fc06(REG_TAGS + 2 * DO0, 1))
    check(e == 3, f"FC06 on the tag area -> 0x03 (got {e})")
    e = exc_code(d.fc16(REG_TAGS + 2 * DO0, [0, 1, 0]))
    check(e == 3, f"odd quantity -> 0x03 (got {e})")
    e = exc_code(d.fc16(REG_TAGS + 2 * DO0 + 1, [0, 1]))
    check(e == 3, f"start on a low-word address -> 0x03 (got {e})")

    print("5. All-or-nothing")
    if populated < MAX_TAGS:
        CNLAST = base_counter + counter - 1
        d.write_tag(CNLAST, 0)
        e = exc_code(d.fc16(REG_TAGS + 2 * CNLAST, [0, 7, 0, 7]))
        check(e == 2, f"[COUNTER{counter - 1}, unpopulated] span rejected with 0x02 (got {e})")
        check(d.tag(CNLAST) == 0, f"COUNTER{counter - 1} NOT modified by the rejected frame")
    else:
        print("  (board has no unpopulated slot, skipped)")

    print("6. Retain draft (step 4)")
    orig = d.tag(RT0)
    check(exc_code(d.write_tag(RT0, orig + 111)) is None, "VREG_RETAIN0 write accepted")
    check(d.tag(RT0) == orig + 111, "read-back shows the draft value")
    check(d.read(REG_DIAG + 2, 1)[0] & 1 == 1, "RETAIN_DIRTY set")
    d.fc06(REG_DIAG, CMD_EXIT)
    time.sleep(0.1)
    check(d.state() == STATE_CONTROL and d.read(REG_DIAG + 4, 1)[0] == 5,
          "EXIT refused while dirty (ERR_RETAIN_DIRTY)")
    d.fc16(REG_DIAG, [CMD_HEARTBEAT])
    d.fc06(REG_DIAG, CMD_DISCARD)
    time.sleep(0.1)
    check(d.read(REG_DIAG + 2, 1)[0] & 1 == 0, "DISCARD clears RETAIN_DIRTY")
    check(d.tag(RT0) == orig, f"old value back after DISCARD (got {d.tag(RT0)}, expected {orig})")
    check(d.read(REG_DIAG + 4, 1)[0] == 0, "ERR_RETAIN_DIRTY cleared")
    if do_commit:
        print("   committing to Flash (2 records) ...")
        d.write_tag(RT0, 4242)
        d.fc06(REG_DIAG, CMD_COMMIT)
        time.sleep(0.2)
        check(d.read(REG_DIAG + 2, 1)[0] & 1 == 0, "COMMIT clears RETAIN_DIRTY")
        check(d.read(REG_DIAG + 4, 1)[0] == 0, "no error after COMMIT")
        check(d.tag(RT0) == 4242, "committed value reads back 4242")
        d.fc16(REG_DIAG, [CMD_HEARTBEAT])
        d.write_tag(RT0, orig)
        d.fc06(REG_DIAG, CMD_COMMIT)
        time.sleep(0.2)
        check(d.tag(RT0) == orig, f"original value {orig} restored and committed")
    d.fc16(REG_DIAG, [CMD_HEARTBEAT])

    print("7. EXIT_DIAG keeps the values")
    d.fc06(REG_DIAG, CMD_EXIT)
    time.sleep(0.2)
    check(d.state() == STATE_RUNNING, "back in ENGINE_RUNNING")
    check(d.tag(VR0) == -123456, "VREG0 kept after EXIT (no rule touches it)")
    print("   note: DO0 may change now -- the Rule Engine owns it again.")

    if not do_reboot:
        print("\n(skipped Step 5: add --reboot to test the baseline reset)")
        return

    print("8. Step 5: baseline reset before SYSTEM_COMMAND REBOOT")
    d.fc06(REG_DIAG, CMD_ENTER)
    t0 = time.time()
    while d.state() != STATE_CONTROL and time.time() - t0 < 1.0:
        time.sleep(0.02)
    d.write_tag(DO0, 1)
    d.write_tag(VF0, 5)
    check(d.tag(DO0) == 1 and d.tag(VF0) == 5, "DO0=1, VFLAG0=5 set in diag")
    print("   sending REBOOT (the board will reset ~300 ms later) ...")
    d.fc06(REG_SYSCMD, SYSCMD_REBOOT)
    try:
        ok = d.tag(DO0) == 0 and d.tag(VF0) == 0
        check(ok, "DO0 and VFLAG0 already 0 right after the command")
    except Exception as ex:   # the MCU may already be gone
        print(f"  [INFO] board reset before the read-back ({ex}); check that the DO0 output went OFF")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--pins", action="store_true", help="pause so you can check the physical DO0")
    p.add_argument("--pin-seconds", type=float, default=5.0, help="length of the --pins pause (default 5)")
    p.add_argument("--commit", action="store_true", help="also test COMMIT_RETAIN (writes Flash)")
    p.add_argument("--reboot", action="store_true", help="also test the Step 5 baseline reset + REBOOT")
    a = p.parse_args()
    global PIN_SECONDS
    PIN_SECONDS = a.pin_seconds

    client = ModbusSerialClient(port=a.port, baudrate=a.baudrate, timeout=1.0)
    if not client.connect():
        print(f"Cannot open {a.port}")
        return 2
    try:
        run(Dev(client, a.unit), a.pins, a.reboot, a.commit)
    finally:
        client.close()
    print("\nALL PASS" if _failures == 0 else f"\n{_failures} FAILED")
    return 0 if _failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
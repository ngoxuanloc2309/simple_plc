#!/usr/bin/env python3
"""
test_diag.py - Verify SimplePLC's Diagnostic Control block (Wire Profile V2,
0x0A20..0x0A24) over Modbus RTU / USB-CDC. Step 2 of the V2.0 plan: state
machine + lease only -- runtime tag writes (0x0900..0x09FF) are NOT tested
here because the firmware does not allow them yet.

Requires: pip install pymodbus pyserial   (same as test_plc.py)

What this checks, in order (each line prints PASS/FAIL):
  1. Idle state: DIAG_STATE=ENGINE_RUNNING, no flags, lease 0.
  2. CMD_ENTER_DIAG (FC06): reaches DIAG_CONTROL within 1000 ms, LEASE_ACTIVE
     set, lease ~3000 ms, DIAG_ERROR_CODE cleared.
  3. Lease counts down on its own, and a repeated ENTER does NOT reset it.
  4. FC16 with quantity > 1 at 0x0A20 is rejected (Modbus exception 0x03)
     and nothing is executed.
  5. Writing a read-only diag register (0x0A21) is rejected.
  6. CMD_HEARTBEAT (FC16, quantity 1) renews the lease.
  7. Unknown command (9) latches ERR_INVALID_COMMAND and keeps the session.
  8. CMD_EXIT_DIAG returns to ENGINE_RUNNING.
  9. Lease expiry (skip with --skip-expiry, takes ~4 s): after ENTER and no
     heartbeat, the MCU falls back to ENGINE_RUNNING by itself and latches
     ERR_LEASE_EXPIRED, which survives a late HEARTBEAT.

Manual check worth doing once with a rule loaded (e.g. test_plc.py's
DI0 -> DO0): ENTER_DIAG, then toggle DI0 -- DO0 must NOT react while in
DIAG_CONTROL (Rule Engine suspended). After EXIT_DIAG (or lease expiry) the
rule must work again.
"""

import argparse
import inspect
import sys
import time

from pymodbus.client import ModbusSerialClient

REG_DIAG_BASE = 0x0A20  # COMMAND, STATE, FLAGS, LEASE_MS, ERROR_CODE

CMD_ENTER, CMD_HEARTBEAT, CMD_EXIT, CMD_COMMIT, CMD_DISCARD = 1, 2, 3, 4, 5
STATE_RUNNING, STATE_CONTROL = 1, 2
FLAG_RETAIN_DIRTY, FLAG_LEASE_ACTIVE = 0x1, 0x2
ERR_NONE, ERR_LEASE_EXPIRED, ERR_INVALID_COMMAND = 0, 2, 4

_failures = 0


def check(cond, msg):
    global _failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
    if not cond:
        _failures += 1
    return cond


def _unit_kwarg(client, unit):
    # pymodbus renamed slave= -> device_id= in 3.10; same detection as test_plc.py.
    params = inspect.signature(client.read_holding_registers).parameters
    if "device_id" in params:
        return {"device_id": unit}
    if "slave" in params:
        return {"slave": unit}
    raise RuntimeError("Unsupported pymodbus version")


class Diag:
    def __init__(self, client, unit):
        self.c, self.kw = client, _unit_kwarg(client, unit)

    def block(self):
        rr = self.c.read_holding_registers(address=REG_DIAG_BASE, count=5, **self.kw)
        if rr.isError():
            raise RuntimeError(f"read 0x{REG_DIAG_BASE:04X} failed: {rr}")
        cmd, state, flags, lease, err = rr.registers
        return dict(cmd=cmd, state=state, flags=flags, lease=lease, err=err)

    def fc06(self, addr, value):
        return self.c.write_register(address=addr, value=value, **self.kw)

    def fc16(self, addr, values):
        return self.c.write_registers(address=addr, values=values, **self.kw)

    def wait_state(self, state, timeout_s=1.0):
        t0 = time.time()
        while time.time() - t0 < timeout_s:
            if self.block()["state"] == state:
                return True
            time.sleep(0.02)
        return False


def run(d, skip_expiry):
    print("1. Idle state")
    b = d.block()
    check(b["state"] == STATE_RUNNING, f"DIAG_STATE == ENGINE_RUNNING (got {b['state']})")
    check(b["flags"] == 0 and b["lease"] == 0, f"flags=0, lease=0 (got {b['flags']}, {b['lease']})")
    if b["state"] != STATE_RUNNING:
        print("  Device is not idle (previous session?). Waiting for lease expiry ...")
        time.sleep(3.5)

    print("2. ENTER_DIAG (FC06)")
    r = d.fc06(REG_DIAG_BASE, CMD_ENTER)
    check(not r.isError(), "ENTER acknowledged")
    check(d.wait_state(STATE_CONTROL), "reached DIAG_CONTROL within 1000 ms")
    b = d.block()
    check(b["flags"] & FLAG_LEASE_ACTIVE, f"LEASE_ACTIVE set (flags=0x{b['flags']:04X})")
    check(2000 <= b["lease"] <= 3000, f"lease ~3000 ms (got {b['lease']})")
    check(b["err"] == ERR_NONE, f"error code cleared (got {b['err']})")
    check(b["cmd"] == CMD_ENTER, f"command reads back last value (got {b['cmd']})")

    print("3. Lease counts down; repeated ENTER is a no-op")
    l1 = d.block()["lease"]
    time.sleep(0.5)
    d.fc06(REG_DIAG_BASE, CMD_ENTER)
    l2 = d.block()["lease"]
    check(l2 < l1, f"lease decreased without heartbeat ({l1} -> {l2})")
    check(l2 < 2800, f"second ENTER did not reset the lease (lease={l2})")

    print("4. FC16 quantity > 1 at 0x0A20 is rejected")
    r = d.fc16(REG_DIAG_BASE, [CMD_EXIT, 0])
    check(r.isError(), "FC16 quantity=2 rejected")
    check(d.block()["state"] == STATE_CONTROL, "state unchanged (EXIT was not executed)")

    print("5. Read-only diag register")
    r = d.fc06(REG_DIAG_BASE + 1, STATE_RUNNING)
    check(r.isError(), "FC06 to DIAG_STATE (0x0A21) rejected")

    print("6. HEARTBEAT (FC16, quantity 1)")
    r = d.fc16(REG_DIAG_BASE, [CMD_HEARTBEAT])
    check(not r.isError(), "HEARTBEAT acknowledged")
    lease = d.block()["lease"]
    check(lease >= 2900, f"lease renewed (got {lease})")

    print("7. Unknown command")
    d.fc06(REG_DIAG_BASE, 9)
    b = d.block()
    check(b["err"] == ERR_INVALID_COMMAND, f"ERR_INVALID_COMMAND latched (got {b['err']})")
    check(b["state"] == STATE_CONTROL, "session kept")
    d.fc06(REG_DIAG_BASE, CMD_HEARTBEAT)
    check(d.block()["err"] == ERR_INVALID_COMMAND, "HEARTBEAT does not clear the latched error")

    print("8. EXIT_DIAG")
    d.fc06(REG_DIAG_BASE, CMD_EXIT)
    check(d.wait_state(STATE_RUNNING), "back to ENGINE_RUNNING within 1000 ms")
    b = d.block()
    check(b["flags"] == 0 and b["lease"] == 0, "flags and lease cleared")

    if skip_expiry:
        print("9. Lease expiry: skipped")
        return

    print("9. Lease expiry (waiting ~3.6 s, no heartbeat)")
    d.fc06(REG_DIAG_BASE, CMD_ENTER)
    check(d.wait_state(STATE_CONTROL), "entered DIAG_CONTROL")
    time.sleep(3.6)
    b = d.block()
    check(b["state"] == STATE_RUNNING, f"fell back to ENGINE_RUNNING (got {b['state']})")
    check(b["err"] == ERR_LEASE_EXPIRED, f"ERR_LEASE_EXPIRED latched (got {b['err']})")
    check(b["lease"] == 0 and b["flags"] == 0, "lease and flags cleared")
    d.fc06(REG_DIAG_BASE, CMD_HEARTBEAT)
    check(d.block()["err"] == ERR_LEASE_EXPIRED, "late HEARTBEAT keeps ERR_LEASE_EXPIRED")
    d.fc06(REG_DIAG_BASE, CMD_ENTER)
    check(d.block()["err"] == ERR_NONE, "new ENTER clears the latched error")
    d.fc06(REG_DIAG_BASE, CMD_EXIT)
    check(d.wait_state(STATE_RUNNING), "left diag mode cleanly")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="Serial port, e.g. COM5 or /dev/ttyACM0")
    p.add_argument("--unit", type=int, default=1, help="Modbus unit id (default 1)")
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--skip-expiry", action="store_true", help="Skip the ~4 s lease-expiry test")
    a = p.parse_args()

    client = ModbusSerialClient(port=a.port, baudrate=a.baudrate, bytesize=8,
                                parity="N", stopbits=1, timeout=1)
    if not client.connect():
        print(f"Failed to open {a.port}")
        sys.exit(1)
    try:
        run(Diag(client, a.unit), a.skip_expiry)
    finally:
        try:  # never leave the board in DIAG_CONTROL (Rule Engine suspended)
            Diag(client, a.unit).fc06(REG_DIAG_BASE, CMD_EXIT)
        except Exception:
            pass
        client.close()

    print(f"\n{'ALL PASS' if _failures == 0 else str(_failures) + ' FAILED'}")
    sys.exit(0 if _failures == 0 else 1)


if __name__ == "__main__":
    main()
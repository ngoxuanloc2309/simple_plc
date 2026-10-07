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

Manual mode (python test_diag.py COM5 manual): checks that the Rule Engine
is really suspended during DIAG_CONTROL. Needs a rule such as test_plc.py's
"IF DI0 rises THEN DO0 = 1" already loaded. The script enters diag mode,
keeps it alive with a heartbeat every second, and prints DI0..DIn / DO0
whenever they change. While it says "DIAG_CONTROL", toggling DI0 must NOT
change DO0. After it leaves diag mode, DI0 must drive DO0 again.
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
SPLC_HEARTBEAT_S = 1.0


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


REG_RESOURCE_DI_COUNT = 0x0020 + 3   # DEVICE_RESOURCE_INFO.di_count
REG_RUNTIME_TAGS = 0x0900            # int32 per tag, high word first


def read_tag(d, idx):
    rr = d.c.read_holding_registers(address=REG_RUNTIME_TAGS + idx * 2, count=2, **d.kw)
    if rr.isError():
        raise RuntimeError(f"read tag {idx} failed: {rr}")
    v = (rr.registers[0] << 16) | rr.registers[1]
    return v - (1 << 32) if v & 0x80000000 else v


def watch(d, seconds, di_count, tag_do0, label):
    print(f"--- {label} ({seconds:.0f} s): toggle DI0 now ---")
    t_end, last, next_hb = time.time() + seconds, None, 0.0
    while time.time() < t_end:
        if label.startswith("DIAG") and time.time() >= next_hb:
            d.fc06(REG_DIAG_BASE, CMD_HEARTBEAT)
            next_hb = time.time() + SPLC_HEARTBEAT_S
        row = tuple(read_tag(d, i) for i in range(di_count)) + (read_tag(d, tag_do0),)
        if row != last:
            di = " ".join(f"DI{i}={v}" for i, v in enumerate(row[:-1]))
            print(f"  [{label}] {di}  DO0={row[-1]}")
            last = row
        time.sleep(0.1)


def run_manual(d, seconds):
    (di_count,) = d.c.read_holding_registers(address=REG_RESOURCE_DI_COUNT, count=1,
                                             **d.kw).registers
    tag_do0 = di_count   # DO tags start right after DI, same as test_plc.py
    print(f"di_count={di_count}, DO0 is tag {tag_do0}")
    watch(d, seconds, di_count, tag_do0, "NORMAL: rule should drive DO0")
    d.fc06(REG_DIAG_BASE, CMD_ENTER)
    if not d.wait_state(STATE_CONTROL):
        print("Could not enter DIAG_CONTROL"); return
    watch(d, seconds, di_count, tag_do0, "DIAG_CONTROL: DO0 must NOT change")
    d.fc06(REG_DIAG_BASE, CMD_EXIT)
    d.wait_state(STATE_RUNNING)
    watch(d, seconds, di_count, tag_do0, "NORMAL again: rule should drive DO0")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="Serial port, e.g. COM5 or /dev/ttyACM0")
    p.add_argument("mode", nargs="?", default="auto", choices=["auto", "manual"],
                   help="'auto' (default): protocol checks. 'manual': Rule Engine suspension test by hand")
    p.add_argument("--seconds", type=float, default=10.0,
                   help="manual mode: length of each of the 3 phases (default 10)")
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
        if a.mode == "manual":
            run_manual(Diag(client, a.unit), a.seconds)
        else:
            run(Diag(client, a.unit), a.skip_expiry)
    finally:
        try:  # never leave the board in DIAG_CONTROL (Rule Engine suspended)
            Diag(client, a.unit).fc06(REG_DIAG_BASE, CMD_EXIT)
        except Exception:
            pass
        client.close()

    if a.mode == "auto":
        print(f"\n{'ALL PASS' if _failures == 0 else str(_failures) + ' FAILED'}")
        sys.exit(0 if _failures == 0 else 1)


if __name__ == "__main__":
    main()
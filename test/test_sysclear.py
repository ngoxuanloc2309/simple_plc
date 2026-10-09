#!/usr/bin/env python3
"""
test_sysclear.py - Verify SYSTEM_COMMAND CLEAR_RULES / CLEAR_RETAIN /
FACTORY_RESET (Wire Profile V2, plan step 6) over Modbus RTU / USB-CDC.

Requires: pip install pymodbus pyserial   (same as test_plc.py / test_tag.py)
Put this file next to test_plc.py (it reuses the rule-staging helper).

Board-agnostic: tag indices come from DeviceResourceInfo (0x0020), dense
layout (DI, DO, AI, VFLAG, VREG, VREG_RETAIN, COUNTER) like plc_tag.c.

WARNING: this test WRITES Flash (rule table A/B + retain log) and ERASES the
rule table and all retain values on the board. Do not run it on a board
whose rules you want to keep.

Checks, in order:
  1. CLEAR_RULES : upload 1 rule -> RULE_TABLE_INFO=1 -> CLEAR_RULES ->
                   result DONE/NONE, RULE_TABLE_INFO=0, ACTIVE_RULE_COUNT=0,
                   first rule slot (0x0100..0x010F) all zero.
  2. CLEAR_RETAIN: RETAIN0 = 0x1234 committed via diag -> CLEAR_RETAIN ->
                   result DONE, ALL retain tags read 0, VREG (volatile) NOT
                   touched.
  3. FACTORY_RESET: rule + retain set again -> FACTORY_RESET -> both empty.
  4. Diag interplay: ENTER_DIAG, DO0=1, CLEAR_RULES -> DO0 reads 0 (step 5
                   baseline reset), command still reaches DONE.
  5. Bad command value (9) -> result ERROR / INVALID_COMMAND (or exception).

Optional:
  --reboot   Persistence across a reset (the real proof the data left Flash).
             Before each clear it checks the data SURVIVES a REBOOT (so the
             test is meaningful), then clears, reboots again and checks the
             data is still gone. The COM port drops during reboot; the script
             reconnects (up to 20 s). If Windows gives the board a NEW COM
             number after re-enumeration, pass --port-after COMx.

Usage:
  python test_sysclear.py COM14
  python test_sysclear.py COM14 --reboot
"""

import argparse
import inspect
import sys
import time

from pymodbus.client import ModbusSerialClient

import test_plc as tp  # rule encoding/staging helpers

REG_RESOURCE = 0x0020
REG_RULE_INFO = 0x0010
REG_ACTIVE_RULE = 0x0100
REG_TAGS = 0x0900
REG_SYSCMD = 0x0A00
REG_SYSRES = 0x0A01
REG_DIAG = 0x0A20
REG_ACTIVE_COUNT = 0x9004

CMD_ENTER, CMD_HEARTBEAT, CMD_EXIT, CMD_COMMIT, CMD_DISCARD = 1, 2, 3, 4, 5
STATE_RUNNING, STATE_CONTROL = 1, 2
SYS_REBOOT, SYS_FACTORY, SYS_CLEAR_RULES, SYS_CLEAR_RETAIN = 1, 2, 3, 4
ST_IDLE, ST_ACCEPTED, ST_BUSY, ST_DONE, ST_ERROR = 0, 1, 2, 3, 4
ERR_NONE, ERR_INVALID_COMMAND, ERR_FLASH = 0, 1, 6
ST_NAMES = {0: "IDLE", 1: "ACCEPTED", 2: "BUSY", 3: "DONE", 4: "ERROR"}

_failures = 0


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
        self.c, self.unit = client, unit
        self.kw = _unit_kwarg(client, unit)

    def read(self, addr, count):
        rr = self.c.read_holding_registers(address=addr, count=count, **self.kw)
        if rr.isError():
            raise RuntimeError(f"read 0x{addr:04X} x{count} failed: {rr}")
        return rr.registers

    def fc06(self, addr, value):
        return self.c.write_register(address=addr, value=value, **self.kw)

    def fc16(self, addr, values):
        return self.c.write_registers(address=addr, values=values, **self.kw)

    def diag_state(self):
        return self.read(REG_DIAG + 1, 1)[0]

    def tag(self, idx):
        hi, lo = self.read(REG_TAGS + 2 * idx, 2)
        v = (hi << 16) | lo
        return v - (1 << 32) if v & 0x80000000 else v

    def write_tag(self, idx, value):
        v = value & 0xFFFFFFFF
        return self.fc16(REG_TAGS + 2 * idx, [v >> 16, v & 0xFFFF])

    def rule_count(self):
        return self.read(REG_RULE_INFO, 1)[0]

    def sys_result(self):
        r = self.read(REG_SYSRES, 2)
        return r[0], r[1]

    def run_syscmd(self, cmd, timeout=3.0):
        """Send SYSTEM_COMMAND and poll the result until it leaves ACCEPTED/BUSY."""
        resp = self.fc06(REG_SYSCMD, cmd)
        if resp.isError():
            return None, None, resp
        t0 = time.time()
        st, err = self.sys_result()
        while st in (ST_ACCEPTED, ST_BUSY) and time.time() - t0 < timeout:
            time.sleep(0.05)
            st, err = self.sys_result()
        return st, err, resp


def exc_code(resp):
    if not resp.isError():
        return None
    return getattr(resp, "exception_code", -1)


def ensure_running(d):
    if d.diag_state() != STATE_RUNNING:
        d.fc06(REG_DIAG, CMD_EXIT)
        time.sleep(0.2)


def set_retain_committed(d, idx, value):
    """ENTER_DIAG, write retain tag (RAM draft), COMMIT_RETAIN, EXIT_DIAG."""
    d.fc06(REG_DIAG, CMD_ENTER)
    time.sleep(0.05)
    ok1 = exc_code(d.write_tag(idx, value)) is None
    d.fc06(REG_DIAG, CMD_COMMIT)
    time.sleep(0.2)
    d.fc06(REG_DIAG, CMD_EXIT)
    time.sleep(0.2)
    return ok1 and d.diag_state() == STATE_RUNNING


def reboot_and_reconnect(d, client_holder, args):
    """Send REBOOT, then reconnect. Returns a new Dev (or None on failure)."""
    print("  ... REBOOT, waiting for the board to come back")
    try:
        d.fc06(REG_SYSCMD, SYS_REBOOT)
    except Exception:
        pass
    time.sleep(0.5)
    try:
        client_holder[0].close()
    except Exception:
        pass
    port = args.port_after or args.port
    deadline = time.time() + 20.0
    while time.time() < deadline:
        time.sleep(1.0)
        try:
            c = ModbusSerialClient(port=port, baudrate=args.baudrate, parity="N",
                                   stopbits=1, bytesize=8, timeout=1.0)
            if not c.connect():
                continue
            nd = Dev(c, args.unit)
            nd.read(0x0000, 1)  # alive?
            client_holder[0] = c
            time.sleep(0.3)
            return nd
        except Exception:
            continue
    print("  could not reconnect (new COM number? use --port-after COMx)")
    return None


def upload_rule(d, tag_do0):
    """Stage + commit 1 rule via test_plc's helper (prints its own log)."""
    tp.stage_and_commit_test_rule(d.c, d.unit, tag_do0)
    time.sleep(0.2)


def run(d, holder, args):
    r = d.read(REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    base_do = di
    base_ai = base_do + do
    base_vflag = base_ai + ai
    base_vreg = base_vflag + vflag
    base_retain = base_vreg + vreg
    print(f"di={di} do={do} ai={ai} vflag={vflag} vreg={vreg} retain={retain} counter={counter}")
    print(f"DO0=tag {base_do}, VREG0=tag {base_vreg}, RETAIN0=tag {base_retain}")
    if do < 1 or vreg < 1 or retain < 1:
        print("Board lacks DO / VREG / RETAIN groups. Aborting.")
        return
    DO0, VR0, RT0 = base_do, base_vreg, base_retain
    retain_tags = list(range(base_retain, base_retain + retain))
    ensure_running(d)

    def all_retain_zero():
        return all(d.tag(i) == 0 for i in retain_tags)

    # ------------------------------------------------------------------ 1
    print("\n1. CLEAR_RULES")
    upload_rule(d, DO0)
    check(d.rule_count() == 1, "RULE_TABLE_INFO == 1 after upload")
    if args.reboot:
        nd = reboot_and_reconnect(d, holder, args)
        if nd is None:
            return
        d = nd
        check(d.rule_count() == 1, "precondition: rule survives REBOOT (Flash works)")
    st, err, resp = d.run_syscmd(SYS_CLEAR_RULES)
    check(st == ST_DONE and err == ERR_NONE,
          f"CLEAR_RULES result = {ST_NAMES.get(st, st)}/err {err} (expect DONE/0)")
    check(d.rule_count() == 0, "RULE_TABLE_INFO == 0")
    check(d.read(REG_ACTIVE_COUNT, 1)[0] == 0, "ACTIVE_RULE_COUNT (0x9004) == 0")
    check(all(v == 0 for v in d.read(REG_ACTIVE_RULE, 16)), "rule slot 0 (0x0100..0x010F) all zero")
    if args.reboot:
        nd = reboot_and_reconnect(d, holder, args)
        if nd is None:
            return
        d = nd
        check(d.rule_count() == 0, "rule table still EMPTY after REBOOT (cleared in Flash)")

    # ------------------------------------------------------------------ 2
    print("\n2. CLEAR_RETAIN")
    ensure_running(d)
    # volatile VREG must NOT be touched by CLEAR_RETAIN: set it in diag first
    d.fc06(REG_DIAG, CMD_ENTER)
    time.sleep(0.05)
    check(exc_code(d.write_tag(VR0, 777)) is None, "VREG0 = 777 written in diag")
    d.fc06(REG_DIAG, CMD_EXIT)  # no retain draft, EXIT allowed
    time.sleep(0.2)
    # Exiting diag resets rule runtime only; VREG0 value stays (rule table is empty)
    check(set_retain_committed(d, RT0, 0x1234), "RETAIN0 = 0x1234 committed")
    check(d.tag(RT0) == 0x1234, "RETAIN0 reads 0x1234")
    if args.reboot:
        nd = reboot_and_reconnect(d, holder, args)
        if nd is None:
            return
        d = nd
        check(d.tag(RT0) == 0x1234, "precondition: RETAIN0 survives REBOOT (Flash works)")
        ensure_running(d)
    st, err, resp = d.run_syscmd(SYS_CLEAR_RETAIN)
    check(st == ST_DONE and err == ERR_NONE,
          f"CLEAR_RETAIN result = {ST_NAMES.get(st, st)}/err {err} (expect DONE/0)")
    check(all_retain_zero(), f"all {retain} retain tag(s) read 0")
    if not args.reboot:
        check(d.tag(VR0) == 777, "VREG0 (volatile) untouched = 777")
    if args.reboot:
        nd = reboot_and_reconnect(d, holder, args)
        if nd is None:
            return
        d = nd
        check(all_retain_zero(), "retain still ZERO after REBOOT (cleared in Flash)")
        check(d.tag(VR0) == 0, "VREG0 back to 0 after reboot (volatile, expected)")

    # ------------------------------------------------------------------ 3
    print("\n3. FACTORY_RESET (rules + retain)")
    ensure_running(d)
    upload_rule(d, DO0)
    check(set_retain_committed(d, RT0, 0x5678), "RETAIN0 = 0x5678 committed")
    check(d.rule_count() == 1 and d.tag(RT0) == 0x5678, "precondition: 1 rule + RETAIN0=0x5678")
    st, err, resp = d.run_syscmd(SYS_FACTORY)
    check(st == ST_DONE and err == ERR_NONE,
          f"FACTORY_RESET result = {ST_NAMES.get(st, st)}/err {err} (expect DONE/0)")
    check(d.rule_count() == 0, "rule table empty")
    check(all_retain_zero(), "all retain tags 0")
    if args.reboot:
        nd = reboot_and_reconnect(d, holder, args)
        if nd is None:
            return
        d = nd
        check(d.rule_count() == 0, "rule table still empty after REBOOT")
        check(all_retain_zero(), "retain still zero after REBOOT")

    # ------------------------------------------------------------------ 4
    print("\n4. SYSTEM_COMMAND while in DIAG_CONTROL (step 5 + step 6 together)")
    ensure_running(d)
    upload_rule(d, DO0)
    d.fc06(REG_DIAG, CMD_ENTER)
    time.sleep(0.05)
    check(exc_code(d.write_tag(DO0, 1)) is None, "DO0 = 1 written in diag")
    check(d.tag(DO0) == 1, "DO0 reads 1")
    st, err, resp = d.run_syscmd(SYS_CLEAR_RULES)
    check(st == ST_DONE and err == ERR_NONE,
          f"CLEAR_RULES in diag result = {ST_NAMES.get(st, st)}/err {err}")
    check(d.tag(DO0) == 0, "DO0 reads 0 (baseline reset ran before the command)")
    check(d.rule_count() == 0, "rule table empty")
    print(f"  (info) DIAG_STATE after the command = {d.diag_state()} "
          "(1=ENGINE_RUNNING, 2=DIAG_CONTROL) - not asserted")
    d.fc06(REG_DIAG, CMD_EXIT)
    time.sleep(0.2)
    ensure_running(d)

    # ------------------------------------------------------------------ 5
    print("\n5. Invalid SYSTEM_COMMAND value")
    resp = d.fc06(REG_SYSCMD, 9)
    if resp.isError():
        check(True, f"value 9 rejected with Modbus exception {exc_code(resp)}")
    else:
        time.sleep(0.1)
        st, err = d.sys_result()
        check(st == ST_ERROR and err == ERR_INVALID_COMMAND,
              f"value 9 -> result {ST_NAMES.get(st, st)}/err {err} (expect ERROR/1)")

    # restore a sane state
    print("\nCleanup: board left with an empty rule table and zero retain.")


def main():
    global _failures
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--reboot", action="store_true",
                   help="also verify persistence across REBOOT (recommended)")
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
        run(Dev(client, args.unit), holder, args)
    finally:
        try:
            holder[0].close()
        except Exception:
            pass
    print("\nALL PASS" if _failures == 0 else f"\n{_failures} CHECK(S) FAILED")
    sys.exit(0 if _failures == 0 else 1)


if __name__ == "__main__":
    main()
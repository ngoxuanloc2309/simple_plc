#!/usr/bin/env python3
"""
test_systag.py - Verify the System Tags of SimplePLC (ICD SPLC-SPEC-SYSTEM-TAGS-006,
Structs doc V2.0 section 3.4.1): 8 read-only tags 1000..1007 mirrored at Modbus
0x0820..0x0827 (FC03).

    0x0820 SYS_FIRST_SCAN   0x0821 SYS_ON        0x0822 SYS_OFF       0x0823 SYS_CLOCK_1S
    0x0824 SYS_CLOCK_100MS  0x0825 SYS_RTC_HOUR  0x0826 SYS_RTC_MINUTE  0x0827 SYS_COMM_OK

Requires: pip install pymodbus pyserial
Standalone (same style as test_mqtt.py). Firmware must be built with SPLC_SYSTAG_ENABLE=1
(the default).

Target, like the other tests:
    python test_systag.py COM35 --unit 1 --baudrate 115200
    python test_systag.py 192.168.110.96 --unit 1            (Modbus TCP, default port 5020)

What the default run does (no Flash is written):
  0. Probe: 0x0820 x8 readable.
  1. Static values: ON = 1, OFF = 0, FIRST_SCAN = 0 long after boot, every flag 0/1,
     hour 0..23, minute 0..59, COMM_OK 0/1.
  2. SYS_CLOCK_1S: sampled for --seconds s; edges ~500 ms apart, duty ~50 %.
     SYS_CLOCK_100MS: both levels seen (the link is too slow to measure 50 ms edges
     reliably, so only "it toggles" is checked).
  3. SYS_RTC_HOUR / MINUTE = LOCAL time (epoch_utc + tz_offset_min * 60): writes the RTC
     (0x0810..0x0812) with several UTC epoch / timezone pairs, reads the tags back, then a
     minute rollover. The RTC is set to this PC's clock again at the end (--skip-rtc: do not
     touch the RTC; then only the plausibility checks of step 1 apply).
  4. Read-only: FC06 / FC16 into 0x0820..0x0827 are refused and the values are unchanged;
     a read that runs past 0x0827 is served, the unmapped 0x0828 reads as 0 (by design of
     cb_read_holding_registers in plc_modbus_cfg.c: gaps and unmapped registers read as 0).

--rule (WRITES Flash: replaces the rule table and ends with CLEAR_RULES, which also wipes
MQTT/FB config the App deployed. Dev board only):
  5. Rules that read the System Tags: ON_RISE on tag 1003 counts ~1 per second; a guard on
     SYS_ON lets a rule run, a guard on SYS_OFF blocks one; a rule whose ACTION is a System
     Tag is refused at COMMIT (all-or-nothing), the running rules stay.

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open the port, 3 = link error.
NOT checked here (needs a reset, 10 ms window): SYS_FIRST_SCAN = 1 in the very first cycle.
"""
import argparse
import calendar
import inspect
import re
import sys
import time

REG_RESOURCE = 0x0020
REG_RTC = 0x0810
REG_SYSTAG = 0x0820
REG_TAGS = 0x0900
REG_SYSCMD = 0x0A00
REG_SYSRES = 0x0A01
REG_STATUS = 0x9000          # CONFIG_STATUS, 0x9001 = CONFIG_ERROR_CODE
REG_RULE_COUNT_STAGED = 0x9002
REG_EXPECTED_CRC = 0x9003
REG_STAGING = 0x9010
REG_COMMIT = 0xA000
REG_VERSION = 0xA001
COMMIT_MAGIC = 0xA5A5
SYS_CLEAR_RULES = 3
ST_ACCEPTED, ST_BUSY = 1, 2
CFG_READY = 3

TAG_FIRST_SCAN, TAG_ON, TAG_OFF, TAG_CLOCK_1S, TAG_CLOCK_100MS = 1000, 1001, 1002, 1003, 1004
TAG_RTC_HOUR, TAG_RTC_MINUTE, TAG_COMM_OK = 1005, 1006, 1007

TRG_ON_RISE, TRG_INTERVAL = 1, 4
ACT_SET, ACT_INC = 0, 2
OP_NONE = 0
GUARD_NONE = 0x7FFF

_failures = 0


def check(cond, msg):
    global _failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
    if not cond:
        _failures += 1
    return cond


def info(msg):
    print(f"  (info) {msg}")


# ------------------------------------------------------------------- encoding
def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def u32(v):
    v &= 0xFFFFFFFF
    return [v >> 16, v & 0xFFFF]


def encode_rule(trigger_type, compare_op, action_type, trigger_tag, action_tag,
                for_ms=0, action_param=0, thr_lo=0, thr_hi=0, guard=GUARD_NONE, enabled=1):
    """16-register wire image of one rule (same layout as test_mqtt.py)."""
    return [*u32(thr_lo), *u32(thr_hi), *u32(for_ms), *u32(action_param),
            trigger_tag & 0xFFFF, action_tag & 0xFFFF, guard & 0xFFFF,
            (enabled << 8) | trigger_type, (compare_op << 8) | action_type,
            0, 0, 0]


def rule_bytes(regs):
    return b"".join(r.to_bytes(2, "big") for r in regs)


def local_parts(epoch_utc, tz_min):
    """(hour, minute) of epoch_utc in a timezone tz_min minutes east of UTC."""
    sod = (epoch_utc + tz_min * 60) % 86400
    return sod // 3600, (sod % 3600) // 60


def utc_epoch(y, mo, d, h, mi, s):
    return calendar.timegm((y, mo, d, h, mi, s, 0, 0, 0))


# ---------------------------------------------------------------------- link
class LinkError(RuntimeError):
    """A request was lost and its outcome matters. Never a firmware verdict."""


class ErrResp:
    def __init__(self, code=-1, link=False):
        self.exception_code = code
        self.link = link
        self.text = ""

    def isError(self):
        return True

    def __str__(self):
        return self.text or f"exception {self.exception_code}"


def parse_tcp_target(port):
    """(host, tcp_port) for an IP / host:port / tcp://..., None for a serial name."""
    t = str(port).strip()
    if t.lower().startswith("tcp://"):
        t = t[6:]
    elif not (re.match(r"^\d{1,3}(\.\d{1,3}){3}(:\d+)?$", t) or re.match(r"^[A-Za-z0-9_.-]+:\d+$", t)):
        return None
    host, _, prt = t.partition(":")
    return host, (int(prt) if prt else 5020)


def make_client(port, baudrate):
    tcp = parse_tcp_target(port)
    if tcp is not None:
        from pymodbus.client import ModbusTcpClient
        host, tport = tcp
        try:
            return ModbusTcpClient(host, port=tport, timeout=3.0, retries=0)
        except TypeError:
            return ModbusTcpClient(host, port=tport, timeout=3.0)
    from pymodbus.client import ModbusSerialClient
    kw = dict(port=port, baudrate=baudrate, parity="N", stopbits=1, bytesize=8, timeout=1.0)
    try:
        return ModbusSerialClient(retries=0, **kw)
    except TypeError:
        return ModbusSerialClient(**kw)


def _is_io_error(resp):
    return type(resp).__name__ == "ModbusIOException"


def _unit_kwarg(client, unit):
    params = inspect.signature(client.read_holding_registers).parameters
    if "device_id" in params:
        return {"device_id": unit}
    if "slave" in params:
        return {"slave": unit}
    raise RuntimeError("Unsupported pymodbus version")


def exc_code(resp):
    return getattr(resp, "exception_code", -1) if resp.isError() else None


class Dev:
    """Modbus access. Reads and idempotent writes are repeated on a lost frame;
    COMMIT and system commands are not."""
    retries = 2
    write_retries = 2

    def __init__(self, client, unit):
        self.c, self.unit = client, unit
        self.kw = _unit_kwarg(client, unit)

    def try_read(self, addr, count):
        last = None
        for _ in range(Dev.retries + 1):
            try:
                rr = self.c.read_holding_registers(address=addr, count=count, **self.kw)
            except Exception as e:
                last = ErrResp(-1, link=True)
                last.text = str(e)
                time.sleep(0.05)
                continue
            if _is_io_error(rr):
                last = ErrResp(-1, link=True)
                last.text = str(rr)
                time.sleep(0.05)
                continue
            if rr.isError():
                return None, rr
            return list(rr.registers), None
        return None, last

    def read(self, addr, count):
        regs, err = self.try_read(addr, count)
        if err is not None:
            msg = f"read 0x{addr:04X} x{count} failed: {err}"
            if getattr(err, "link", False):
                raise LinkError(msg)
            raise RuntimeError(msg)
        return regs

    def _write(self, call, what, safe):
        tries = (Dev.write_retries if safe else 0) + 1
        last = None
        for _ in range(tries):
            try:
                r = call()
            except Exception as e:
                last = str(e)
                time.sleep(0.05)
                continue
            if _is_io_error(r):
                last = str(r)
                time.sleep(0.05)
                continue
            return r
        raise LinkError(f"{what}: no valid reply ({last})" + ("" if safe else " [not resent]"))

    def fc06(self, addr, value, safe=True):
        return self._write(lambda: self.c.write_register(address=addr, value=value, **self.kw),
                           f"FC06 0x{addr:04X}", safe)

    def fc16(self, addr, values, safe=True):
        return self._write(lambda: self.c.write_registers(address=addr, values=values, **self.kw),
                           f"FC16 0x{addr:04X} x{len(values)}", safe)


# ------------------------------------------------------------------ helpers
def read_tags(d):
    """The 8 registers as a dict keyed by tag index 1000..1007."""
    r = d.read(REG_SYSTAG, 8)
    return {1000 + i: r[i] for i in range(8)}


def read_tag32(d, idx):
    hi, lo = d.read(REG_TAGS + idx * 2, 2)
    v = (hi << 16) | lo
    return v - (1 << 32) if v & 0x80000000 else v


def set_rtc(d, epoch_utc, tz_min):
    return d.fc16(REG_RTC, [(epoch_utc >> 16) & 0xFFFF, epoch_utc & 0xFFFF, tz_min & 0xFFFF])


def pc_tz_minutes():
    """This PC's current UTC offset in minutes east of UTC."""
    off = -(time.altzone if time.localtime().tm_isdst > 0 else time.timezone)
    return off // 60


def commit(d, rules):
    """Stage + COMMIT like the App's Deploy; returns (config_status, error_code)."""
    crc = crc16_modbus(b"".join(rule_bytes(r) for r in rules))
    v0 = d.read(REG_VERSION, 1)[0]
    d.fc06(REG_RULE_COUNT_STAGED, len(rules))
    for i, rule in enumerate(rules):
        d.fc16(REG_STAGING + i * 16, rule)
    d.fc06(REG_EXPECTED_CRC, crc)
    try:
        d.fc06(REG_COMMIT, COMMIT_MAGIC, safe=False)
    except LinkError:
        time.sleep(0.3)
        if d.read(REG_VERSION, 1)[0] == v0:
            raise LinkError("COMMIT lost: outcome unknown (rejected or never received)")
    time.sleep(0.1)
    st, err = d.read(REG_STATUS, 2)
    t0 = time.time()
    while st != CFG_READY and err == 0 and time.time() - t0 < 3.0:
        time.sleep(0.1)
        st, err = d.read(REG_STATUS, 2)
    return st, err


def run_syscmd(d, cmd, timeout=3.0):
    resp = d.fc06(REG_SYSCMD, cmd, safe=True)
    if resp.isError():
        return None, None
    t0 = time.time()
    st, err = d.read(REG_SYSRES, 2)
    while st in (ST_ACCEPTED, ST_BUSY) and time.time() - t0 < timeout:
        time.sleep(0.05)
        st, err = d.read(REG_SYSRES, 2)
    return st, err


# -------------------------------------------------------------------- steps
def step_probe(d):
    print("\n0. Probe 0x0820..0x0827")
    regs, err = d.try_read(REG_SYSTAG, 8)
    if err is not None and getattr(err, "link", False):
        raise LinkError(f"probe 0x0820: {err}")
    if err is not None:
        info(f"0x0820 not readable ({err}): firmware built without SPLC_SYSTAG_ENABLE, "
             f"or an old binary")
        check(False, "0x0820 x8 readable (FC03)")
        return False
    check(True, f"0x0820 x8 readable: {regs}")
    return True


def step_static(d):
    print("\n1. Static values")
    t = read_tags(d)
    check(t[TAG_ON] == 1, "SYS_ON (0x0821) = 1")
    check(t[TAG_OFF] == 0, "SYS_OFF (0x0822) = 0")
    check(t[TAG_FIRST_SCAN] == 0, "SYS_FIRST_SCAN (0x0820) = 0 long after boot")
    check(all(t[i] in (0, 1) for i in (1000, 1001, 1002, 1003, 1004, 1007)),
          "flag registers are 0 or 1 only")
    check(0 <= t[TAG_RTC_HOUR] <= 23, f"SYS_RTC_HOUR 0..23 (read {t[TAG_RTC_HOUR]})")
    check(0 <= t[TAG_RTC_MINUTE] <= 59, f"SYS_RTC_MINUTE 0..59 (read {t[TAG_RTC_MINUTE]})")
    info(f"SYS_COMM_OK = {t[TAG_COMM_OK]} (1 until the host registers a source)")
    for _ in range(20):                       # constants stay constant
        u = read_tags(d)
        if u[TAG_ON] != 1 or u[TAG_OFF] != 0 or u[TAG_FIRST_SCAN] != 0:
            check(False, f"constants changed: {u}")
            return
    check(True, "ON / OFF / FIRST_SCAN stable over 20 reads")


def step_clocks(d, seconds):
    print(f"\n2. Clocks ({seconds:.0f} s of back-to-back reads)")
    samples = []                               # (t, clk1s, clk100ms)
    t_end = time.time() + seconds
    while time.time() < t_end:
        r = d.read(REG_SYSTAG + 3, 2)
        samples.append((time.time(), r[0], r[1]))
    n = len(samples)
    rate = n / seconds
    info(f"{n} samples, {rate:.0f} reads/s")
    edges = []
    for (t0, a0, _), (t1, a1, _) in zip(samples, samples[1:]):
        if a0 != a1:
            edges.append((t0 + t1) / 2.0)
    high = sum(1 for _, a, _ in samples if a == 1) / float(n)
    check(len(edges) >= int(seconds * 2) - 2,
          f"SYS_CLOCK_1S edges: {len(edges)} (expected ~{int(seconds * 2)})")
    if len(edges) >= 3:
        gaps = sorted(b - a for a, b in zip(edges, edges[1:]))
        med = gaps[len(gaps) // 2]
        tol = max(0.12, 3.0 / rate)
        check(abs(med - 0.5) <= tol, f"SYS_CLOCK_1S median half period {med * 1000:.0f} ms "
                                      f"(expected 500 +/- {tol * 1000:.0f})")
    check(0.35 <= high <= 0.65, f"SYS_CLOCK_1S duty {high * 100:.0f} % high (expected ~50)")
    v100 = {s[2] for s in samples}
    check(v100 == {0, 1}, f"SYS_CLOCK_100MS toggles (levels seen: {sorted(v100)})")
    if rate < 40:
        info("link slower than 40 reads/s: the 50 ms period is not measurable here, only the toggling is")


def step_rtc(d):
    print("\n3. SYS_RTC_HOUR / MINUTE = local time (epoch + tz)")
    cases = [
        ("VN 03:00 local",          utc_epoch(2026, 1, 14, 20, 0, 0), 420),      # UTC 20:00 -> 03:00
        ("UTC 12:34",               utc_epoch(2026, 6, 10, 12, 34, 5), 0),
        ("UTC-5 15:30",             utc_epoch(2026, 3, 5, 20, 30, 10), -300),
        ("UTC+7 crosses midnight",  utc_epoch(2026, 1, 15, 23, 59, 30), 420),    # local 06:59, next day
        ("UTC+14 -> 00:15 next day", utc_epoch(2026, 1, 15, 10, 15, 20), 840),
        ("UTC-12 -> 23:xx",         utc_epoch(2026, 1, 15, 11, 5, 5), -720),
    ]
    for name, epoch, tz in cases:
        resp = set_rtc(d, epoch, tz)
        if not check(not resp.isError(), f"{name}: RTC write accepted ({resp if resp.isError() else 'ok'})"):
            continue
        t_set = time.time()
        time.sleep(0.4)
        t = read_tags(d)
        t_read = time.time()
        allowed = {local_parts(epoch + int(t_read - t_set) + k, tz) for k in (-1, 0, 1)}
        got = (t[TAG_RTC_HOUR], t[TAG_RTC_MINUTE])
        check(got in allowed, f"{name}: tags {got[0]:02d}:{got[1]:02d} (expected one of "
                              f"{sorted(f'{h:02d}:{m:02d}' for h, m in allowed)})")
    print("  -- minute rollover (waits ~6 s)")
    epoch = utc_epoch(2026, 1, 15, 16, 59, 56)      # tz +420 -> 23:59:56 local
    set_rtc(d, epoch, 420)
    time.sleep(0.4)
    t = read_tags(d)
    check((t[TAG_RTC_HOUR], t[TAG_RTC_MINUTE]) == (23, 59), f"before: 23:59 (read {t[TAG_RTC_HOUR]:02d}:{t[TAG_RTC_MINUTE]:02d})")
    time.sleep(5.2)
    t = read_tags(d)
    check((t[TAG_RTC_HOUR], t[TAG_RTC_MINUTE]) == (0, 0), f"after ~5.6 s: 00:00 (read {t[TAG_RTC_HOUR]:02d}:{t[TAG_RTC_MINUTE]:02d})")


def restore_rtc(d):
    now = int(time.time())
    tz = pc_tz_minutes()
    resp = set_rtc(d, now, tz)
    info(f"RTC set to this PC's clock again (UTC epoch {now}, tz {tz:+d} min): "
         f"{'ok' if not resp.isError() else resp}")


def step_readonly(d):
    print("\n4. Read-only block and its edges")
    before = read_tags(d)
    r1 = d.fc06(REG_SYSTAG + 1, 0)                       # try to clear SYS_ON
    check(r1.isError(), f"FC06 into 0x0821 is refused (exception {exc_code(r1)})")
    r2 = d.fc16(REG_SYSTAG, [0] * 8)
    check(r2.isError(), f"FC16 0x0820 x8 is refused (exception {exc_code(r2)})")
    r3 = d.fc06(REG_SYSTAG + 2, 1)                       # try to set SYS_OFF
    check(r3.isError(), f"FC06 into 0x0822 is refused (exception {exc_code(r3)})")
    after = read_tags(d)
    check(after[TAG_ON] == 1 and after[TAG_OFF] == 0, "SYS_ON / SYS_OFF unchanged after the write attempts")
    regs, err = d.try_read(REG_SYSTAG + 1, 8)            # 0x0821..0x0828: last one not mapped
    if err is not None and getattr(err, "link", False):
        raise LinkError(f"read past the block: {err}")
    # Firmware design: a read crossing the end of a block is NOT refused; unmapped registers
    # read as 0 (plc_modbus_cfg.c, cb_read_holding_registers). So expect 8 values, the last = 0,
    # and 0x0821..0x0827 equal to the tags read through the normal 8-register read (ON/OFF are static).
    check(err is None and regs is not None and len(regs) == 8 and regs[7] == 0,
          f"a read running past 0x0827 is served, 0x0828 (unmapped) reads 0 (got {regs}, err {err})")
    if err is None and regs is not None and len(regs) == 8:
        check(regs[0] == 1 and regs[1] == 0, "ON/OFF inside the over-long read are 1 / 0")
    regs, err = d.try_read(REG_SYSTAG + 5, 3)            # 0x0825..0x0827: inside
    check(err is None and len(regs) == 3, "a partial read inside the block works (0x0825 x3)")
    _ = before


def step_rules(d):
    print("\n5. Rules that use System Tags (WRITES Flash, ends with CLEAR_RULES)")
    r = d.read(REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    info(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 3:
        check(False, "at least 3 VREG tags needed (counters for the three rules)")
        return
    va = di + do + ai + vflag
    vb, vc = va + 1, va + 2
    negate = 0x8000
    rules = [
        # va: +1 on every rising edge of SYS_CLOCK_1S
        encode_rule(TRG_ON_RISE, OP_NONE, ACT_INC, TAG_CLOCK_1S, va, action_param=1),
        # vb: +1 every 200 ms, but guarded by SYS_OFF (never open) -> must stay 0
        encode_rule(TRG_INTERVAL, OP_NONE, ACT_INC, 0, vb, for_ms=200, action_param=1, guard=TAG_OFF),
        # vc: +1 every 200 ms, guarded by SYS_ON (always open) -> must count
        encode_rule(TRG_INTERVAL, OP_NONE, ACT_INC, 0, vc, for_ms=200, action_param=1, guard=TAG_ON),
    ]
    st, err = commit(d, rules)
    if not check(st == CFG_READY, f"COMMIT of rules with trigger 1003 and guards 1001/1002 (status {st}, error {err})"):
        return
    b0, c0, a0 = read_tag32(d, vb), read_tag32(d, vc), read_tag32(d, va)
    t0 = time.time()
    time.sleep(6.0)
    elapsed = time.time() - t0
    a1, b1, c1 = read_tag32(d, va), read_tag32(d, vb), read_tag32(d, vc)
    da, db, dc = a1 - a0, b1 - b0, c1 - c0
    check(abs(da - elapsed) <= 2.5, f"ON_RISE on SYS_CLOCK_1S: +{da} in {elapsed:.1f} s (expected ~{elapsed:.0f})")
    check(db == 0, f"guard on SYS_OFF keeps the rule closed: +{db} (expected 0)")
    check(dc >= int(elapsed / 0.2 * 0.6), f"guard on SYS_ON lets the rule run: +{dc} in {elapsed:.1f} s "
                                          f"(INTERVAL 200 ms)")
    ver = d.read(REG_VERSION, 1)[0]
    bad = [encode_rule(TRG_ON_RISE, OP_NONE, ACT_SET, TAG_CLOCK_1S, TAG_CLOCK_100MS, action_param=1),
           encode_rule(TRG_ON_RISE, OP_NONE, ACT_INC, TAG_CLOCK_1S, va, action_param=1)]
    st, err = commit(d, bad)
    check(st != CFG_READY, f"a rule whose ACTION is a System Tag is refused (status {st}, error {err})")
    check(d.read(REG_VERSION, 1)[0] == ver, "rule version unchanged after the refused COMMIT (all-or-nothing)")
    time.sleep(1.2)
    a2 = read_tag32(d, va)
    check(a2 > a1, "the running rules still run after the refused COMMIT")
    badg = [encode_rule(TRG_ON_RISE, OP_NONE, ACT_INC, TAG_CLOCK_1S, va, action_param=1, guard=1008)]
    st, err = commit(d, badg)
    check(st != CFG_READY, f"guard 1008 (past the System Tags) is refused (status {st}, error {err})")
    bad_t = [encode_rule(TRG_ON_RISE, OP_NONE, ACT_INC, 1008, va, action_param=1)]
    st, err = commit(d, bad_t)
    check(st != CFG_READY, f"trigger 1008 is refused (status {st}, error {err})")
    _ = negate


def cleanup(d):
    print("\n6. Cleanup (CLEAR_RULES)")
    st, err = run_syscmd(d, SYS_CLEAR_RULES)
    info(f"CLEAR_RULES -> status {st}, error {err}")


# --------------------------------------------------------------------- main
def run(d, args):
    if not step_probe(d):
        return
    step_static(d)
    step_clocks(d, args.seconds)
    if not args.skip_rtc:
        try:
            step_rtc(d)
        finally:
            restore_rtc(d)
    else:
        info("--skip-rtc: step 3 skipped")
    step_readonly(d)
    if args.rule:
        try:
            step_rules(d)
        finally:
            cleanup(d)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="COMx, /dev/ttyUSBx, or IP[:port] / tcp://host[:port] (Modbus TCP, default 5020)")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=6.0, help="clock sampling time (default 6)")
    p.add_argument("--skip-rtc", action="store_true", help="do not write the RTC (skips step 3)")
    p.add_argument("--rule", action="store_true",
                   help="also deploy rules that use the System Tags (writes Flash, ends with CLEAR_RULES)")
    args = p.parse_args()

    client = make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    try:
        run(Dev(client, args.unit), args)
    except LinkError as e:
        print(f"\nLINK ERROR (inconclusive, not a firmware verdict): {e}")
        sys.exit(3)
    finally:
        try:
            client.close()
        except Exception:
            pass
    print("\nALL PASS" if _failures == 0 else f"\n{_failures} CHECK(S) FAILED")
    sys.exit(0 if _failures == 0 else 1)


if __name__ == "__main__":
    main()
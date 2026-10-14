#!/usr/bin/env python3
"""
test_mqtt.py - Verify the MQTT Publish blocks of SimplePLC (Wire Profile V2.0,
plan step E.6) over Modbus RTU: Telemetry blocks 0x0C00..0x0D7F, Broker config
0x0E00..0x0E53 and the read-only status 0x0E54..0x0E5F.

Requires: pip install pymodbus pyserial          (register tests)
          pip install paho-mqtt                  (only for --broker)
Standalone: it does NOT import test_plc.py / test_rtc.py / test_sysclear.py
(test_plc.py in this repo is now the RS485 bring-up probe and no longer has the
rule helpers those scripts need). Firmware must be built with SPLC_MQTT_ENABLE=1.

Target: Gateway over RS485 (App link), e.g.
    python test_mqtt.py COM35 --unit 1 --baudrate 115200

WARNING: this test WRITES Flash (rule table + MQTT config), REPLACES the rule
table and runs CLEAR_RULES (which also wipes the MQTT config and the broker the
App configured). Run it on a dev board only. The broker password it writes is
stored in clear text in Flash until the final CLEAR_RULES.

Safety on a live gateway: the register tests keep the App broker DISABLED
(broker_control bit0 = 0), so the gateway keeps using its own broker. Only
--broker switches the gateway to a test broker (and back at the end).

Semantics checked (docs/plc_mqtt_design.md section 5):
  * FC16/FC06 to the MQTT blocks go to a DRAFT; reads return the RUNNING config.
  * COMMIT (0xA000 = 0xA5A5) validates the WHOLE draft first; any bad block or
    broker rejects the entire COMMIT (CONFIG_STATUS != READY, error code 2) and
    the rules, FB and MQTT stay as they were. The draft is dropped.
  * A block/broker the Deploy did not write KEEPS the running config.
  * Broker password reads back 0. Writes into 0x0E54..0x0E5F are accepted and
    ignored. CLEAR_RULES / FACTORY_RESET clear MQTT; CLEAR_RETAIN does not.

Checks, in order:
  0. MQTT probe (0x0E54 readable), block reads.
  1. Clean start (CLEAR_RULES): every block and the broker disabled.
  2. Draft invisible before COMMIT, visible and byte-exact after it.
  3. Four blocks + broker deploy; password reads 0; unwritten block keeps config.
  4. FC06 keeps the other fields of the block.
  5. COMMIT rejections (bad MODE/PAYLOAD/QOS, tag_count, tag index, interval,
     deadband NaN/negative, topic, template, broker port/TLS/version), each
     all-or-nothing; a disabled block is not checked; draft dropped afterwards.
  6. Address limits and the read-only status area.
  7. Status area sanity.
  8. (--reboot) persistence across REBOOT, CLEAR_RETAIN keeps MQTT, CLEAR_RULES /
     FACTORY_RESET clear it, also after another REBOOT.
  9. (--broker HOST[:PORT]) real publishing through the gateway: PERIODIC / COV /
     ON_CHANGE / Custom + decimals / QoS / retain, no publish while DIAG holds the
     engine (Q4), resume after DIAG, baseline after the broker config changes.
 10. Cleanup (CLEAR_RULES).

Usage:
  python test_mqtt.py COM35
  python test_mqtt.py COM35 --reboot
  python test_mqtt.py COM35 --reboot --broker 192.168.1.10:1883 [--mqtt-user U --mqtt-pass P]
"""
import argparse
import inspect
import json
import math
import queue
import random
import re
import struct
import sys
import time

# ----------------------------------------------------------------- registers
REG_RESOURCE = 0x0020
REG_RULE_INFO = 0x0010
REG_SYSCMD = 0x0A00
REG_SYSRES = 0x0A01
REG_DIAG = 0x0A20
REG_STATUS = 0x9000          # CONFIG_STATUS, 0x9001 = CONFIG_ERROR_CODE
REG_RULE_COUNT_STAGED = 0x9002
REG_EXPECTED_CRC = 0x9003
REG_STAGING = 0x9010
REG_COMMIT = 0xA000
COMMIT_MAGIC = 0xA5A5
REG_TEL = 0x0C00
TEL_BLOCKS, BLOCK_REGS = 4, 96
REG_BROKER = 0x0E00
BROKER_REGS = 96
BK_STATUS_OFF = 84           # +84.. read-only status
REG_BSTATUS = REG_BROKER + BK_STATUS_OFF
REG_TAGS = 0x0900

CMD_ENTER, CMD_HEARTBEAT, CMD_EXIT = 1, 2, 3
SYS_REBOOT, SYS_FACTORY, SYS_CLEAR_RULES, SYS_CLEAR_RETAIN = 1, 2, 3, 4
ST_ACCEPTED, ST_BUSY, ST_DONE = 1, 2, 3
CFG_READY = 3
ERR_INVALID_PARAMETER = 2
EXC_ADDR = 2

MODE_ON_CHANGE, MODE_COV, MODE_PERIODIC = 0, 1, 2
PAY_COMPACT, PAY_RAW, PAY_CUSTOM = 0, 1, 2
TRG_INTERVAL, ACT_INC = 4, 2
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
                for_ms=0, action_param=0, thr_lo=0, thr_hi=0, guard=GUARD_NONE,
                enabled=1):
    """16-register wire image of one rule (same layout as the other tests)."""
    return [*u32(thr_lo), *u32(thr_hi), *u32(for_ms), *u32(action_param),
            trigger_tag & 0xFFFF, action_tag & 0xFFFF, guard & 0xFFFF,
            (enabled << 8) | trigger_type, (compare_op << 8) | action_type,
            0, 0, 0]


def rule_bytes(regs):
    return b"".join(r.to_bytes(2, "big") for r in regs)


def str_regs(s, nchars):
    b = s if isinstance(s, (bytes, bytearray)) else s.encode("ascii")
    assert len(b) <= nchars, f"string too long ({len(b)} > {nchars}): {s!r}"
    b = bytes(b).ljust(nchars, b"\0")
    return [(b[i] << 8) | b[i + 1] for i in range(0, nchars, 2)]


def regs_str(regs):
    b = b"".join(r.to_bytes(2, "big") for r in regs)
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def make_block(enabled=True, mode=MODE_ON_CHANGE, qos=0, retain=False,
               payload=PAY_COMPACT, en_tag=0xFFFF, interval=1000, deadband=0.0,
               tags=(), topic="", template="", reserved=0, tag_count=None,
               ctrl_raw=None, raw_tags=None, topic_bytes=None,
               deadband_bits=None):
    """96 registers of SPLC_MqttTelemetryBlock_t (splc_mqtt_wire.h)."""
    ctrl = ctrl_raw if ctrl_raw is not None else (
        (1 if enabled else 0) | (mode << 1) | (qos << 3) |
        (0x20 if retain else 0) | (payload << 6))
    if raw_tags is None:
        raw_tags = list(tags) + [0xFFFF] * (8 - len(tags))
    if tag_count is None:
        tag_count = len(tags)
    if deadband_bits is None:
        deadband_bits = struct.unpack(">I", struct.pack(">f", deadband))[0]
    regs = [ctrl, en_tag, *u32(interval), *u32(deadband_bits), tag_count,
            *raw_tags, reserved,
            *str_regs(topic_bytes if topic_bytes is not None else topic, 32),
            *str_regs(template, 128)]
    assert len(regs) == BLOCK_REGS, len(regs)
    return regs


def make_broker(enabled=False, tls=False, clean=True, version=0, port=1883,
                keepalive=60, host="", client_id="", user="", password=""):
    """96 registers of SPLC_MqttBrokerConfig_t (status area zero)."""
    ctrl = (1 if enabled else 0) | (2 if tls else 0) | (4 if clean else 0) | (version << 3)
    regs = [ctrl, port, keepalive, 0, *str_regs(host, 64), *str_regs(client_id, 32),
            *str_regs(user, 32), *str_regs(password, 32), *([0] * 12)]
    assert len(regs) == BROKER_REGS, len(regs)
    return regs


# ----------------------------------------------------------------- device I/O
class ErrResp:
    def __init__(self, code=-1):
        self.exception_code = code

    def isError(self):
        return True


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
    """Modbus access with a bounded retry on READS (RS485 bring-up showed rare
    lost frames). The retry count is printed at the end so noise stays visible."""

    chunk = 48          # registers per FC16 when writing blocks (RS485 frame <= 256 B)
    retries = 2
    retried = 0

    def __init__(self, client, unit):
        self.c, self.unit = client, unit
        self.kw = _unit_kwarg(client, unit)

    def try_read(self, addr, count):
        last = None
        for _ in range(Dev.retries + 1):
            try:
                rr = self.c.read_holding_registers(address=addr, count=count, **self.kw)
            except Exception as e:      # timeout / framing
                last = ErrResp(-1)
                last.text = str(e)
                Dev.retried += 1
                time.sleep(0.05)
                continue
            if rr.isError():
                return None, rr
            return list(rr.registers), None
        return None, last

    def read(self, addr, count):
        regs, err = self.try_read(addr, count)
        if err is not None:
            raise RuntimeError(f"read 0x{addr:04X} x{count} failed: {err}")
        return regs

    def fc06(self, addr, value):
        try:
            return self.c.write_register(address=addr, value=value, **self.kw)
        except Exception as e:
            r = ErrResp(-1)
            r.text = str(e)
            return r

    def fc16(self, addr, values):
        try:
            return self.c.write_registers(address=addr, values=values, **self.kw)
        except Exception as e:
            r = ErrResp(-1)
            r.text = str(e)
            return r

    def write_regs(self, addr, regs):
        """FC16 in chunks; returns the first error response or None."""
        for off in range(0, len(regs), self.chunk):
            resp = self.fc16(addr + off, regs[off:off + self.chunk])
            if resp.isError():
                return resp
        return None

    def reboot(self, args):
        print("  ... REBOOT, waiting for the board to come back")
        try:
            self.fc06(REG_SYSCMD, SYS_REBOOT)
        except Exception:
            pass
        time.sleep(0.5)
        try:
            self.c.close()
        except Exception:
            pass
        from pymodbus.client import ModbusSerialClient
        port = args.port_after or args.port
        deadline = time.time() + 25.0
        while time.time() < deadline:
            time.sleep(1.0)
            try:
                c = ModbusSerialClient(port=port, baudrate=args.baudrate, parity="N",
                                       stopbits=1, bytesize=8, timeout=1.0)
                if not c.connect():
                    continue
                nd = Dev(c, args.unit)
                nd.read(0x0000, 1)
                time.sleep(0.5)
                return nd
            except Exception:
                continue
        print("  could not reconnect (new COM number? use --port-after COMx)")
        return None


# ------------------------------------------------------------------ operations
def rule_count(d):
    return d.read(REG_RULE_INFO, 1)[0]


def commit(d, rules):
    """Stage + COMMIT like the App's Deploy; returns (config_status, error_code)."""
    crc = crc16_modbus(b"".join(rule_bytes(r) for r in rules))
    d.fc06(REG_RULE_COUNT_STAGED, len(rules))
    for i, rule in enumerate(rules):
        d.fc16(REG_STAGING + i * 16, rule)
    d.fc06(REG_EXPECTED_CRC, crc)
    d.fc06(REG_COMMIT, COMMIT_MAGIC)
    time.sleep(0.1)
    st, err = d.read(REG_STATUS, 2)
    t0 = time.time()
    while st != CFG_READY and err == 0 and time.time() - t0 < 3.0:
        time.sleep(0.1)
        st, err = d.read(REG_STATUS, 2)
    return st, err


def run_syscmd(d, cmd, timeout=3.0):
    resp = d.fc06(REG_SYSCMD, cmd)
    if resp.isError():
        return None, None
    t0 = time.time()
    st, err = d.read(REG_SYSRES, 2)
    while st in (ST_ACCEPTED, ST_BUSY) and time.time() - t0 < timeout:
        time.sleep(0.05)
        st, err = d.read(REG_SYSRES, 2)
    return st, err


def read_block(d, i):
    return d.read(REG_TEL + i * BLOCK_REGS, BLOCK_REGS)


def read_broker(d):
    return d.read(REG_BROKER, BROKER_REGS)


def broker_cfg_equal(read, written):
    """Config part (0..67) identical, password area reads 0 (never leaves the device)."""
    return read[:68] == written[:68] and all(r == 0 for r in read[68:BK_STATUS_OFF])


def broker_status(d):
    r = d.read(REG_BSTATUS, 12)
    return {"flags": r[0], "latency": r[1], "tx_msgs": (r[2] << 16) | r[3],
            "tx_bytes": (r[4] << 16) | r[5], "last_err": r[6], "reserved": r[7:12]}


def diff_regs(a, b):
    return [i for i in range(min(len(a), len(b))) if a[i] != b[i]]


# ----------------------------------------------------------------------- tests
def run(d, args):
    r = d.read(REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 2:
        print("Need at least 2 VREG tags (dense layout DI, DO, AI, VFLAG, VREG...)")
        return
    va = di + do + ai + vflag          # first VREG tag index
    vb = va + 1
    base = "splctest/%04x" % random.randrange(0x10000)   # topics must stay <= 32 chars

    def noop_rule():
        # Fires every 60 s: counts VREG a by 1. Harmless, but a real rule so the
        # COMMIT has something to stage (also proves rules and MQTT share a record).
        return encode_rule(TRG_INTERVAL, OP_NONE, ACT_INC, 0, va, for_ms=60000, action_param=1)

    # ------------------------------------------------------------------ 0
    print("\n0. MQTT probe")
    regs, err = d.try_read(REG_BSTATUS, 12)
    if err is not None:
        print(f"  0x0E54 not readable ({err}): firmware built with SPLC_MQTT_ENABLE=0?")
        check(False, "MQTT status area readable")
        return
    check(True, "0x0E54..0x0E5F readable (firmware has MQTT)")
    ok = True
    for i in range(TEL_BLOCKS):
        regs, err = d.try_read(REG_TEL + i * BLOCK_REGS, BLOCK_REGS)
        ok = ok and err is None and len(regs) == BLOCK_REGS
    check(ok, "4 telemetry blocks readable (96 registers each)")
    regs, err = d.try_read(REG_BROKER, BROKER_REGS)
    check(err is None, "broker config readable (96 registers)")
    st = broker_status(d)
    info(f"status: flags=0x{st['flags']:04X} (bit0 = host connected to its broker) "
         f"tx={st['tx_msgs']} last_err={st['last_err']}")

    # ------------------------------------------------------------------ 1
    print("\n1. Clean start: CLEAR_RULES")
    s, e = run_syscmd(d, SYS_CLEAR_RULES)
    check(s == ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
    check(all(read_block(d, i)[0] & 1 == 0 for i in range(TEL_BLOCKS)), "all 4 blocks ENABLED=0")
    check(read_broker(d)[0] & 1 == 0, "broker ENABLED=0 (not App-configured)")
    check(rule_count(d) == 0, "rule table empty")

    # ------------------------------------------------------------------ 2
    print("\n2. Draft invisible before COMMIT, byte-exact after it")
    g0 = make_block(True, MODE_PERIODIC, qos=1, interval=2000, tags=[va], topic=base + "/b0")
    resp = d.write_regs(REG_TEL, g0)
    check(resp is None, "FC16 of block 0 accepted (always ACK, content is checked at COMMIT)")
    check(read_block(d, 0)[0] & 1 == 0, "read before COMMIT still returns the RUNNING config (disabled)")
    s, e = commit(d, [noop_rule()])
    check(s == CFG_READY and e == 0, f"COMMIT ok (status {s}, error {e})")
    got = read_block(d, 0)
    check(got == g0, f"block 0 reads back byte-exact after COMMIT {diff_regs(got, g0) or ''}")
    check(rule_count(d) == 1, "rule table has 1 rule")

    # ------------------------------------------------------------------ 3
    print("\n3. Four blocks + broker (App broker DISABLED), password, unwritten block")
    g1 = make_block(True, MODE_COV, qos=0, payload=PAY_RAW, deadband=2.5, tags=[va],
                    topic=base + "/b1")
    g2 = make_block(True, MODE_ON_CHANGE, qos=1, payload=PAY_COMPACT, tags=[va, vb],
                    topic=base + "/b2", en_tag=0xFFFF)
    tmpl = '{"a":{val1},"b":{val2:2},"d":"{device}","ts":{ts},"v":[{vals}]}'
    g3 = make_block(True, MODE_PERIODIC, qos=0, retain=True, payload=PAY_CUSTOM,
                    interval=3000, tags=[va, vb], topic=base + "/b3", template=tmpl, reserved=0x0009)
    brk = make_broker(enabled=False, port=1883, keepalive=30, host="test.invalid",
                      client_id="splc_test", user="user1", password="secret")
    resp = d.write_regs(REG_TEL + 1 * BLOCK_REGS, g1 + g2 + g3)
    check(resp is None, "blocks 1..3 written in one go")
    resp = d.write_regs(REG_BROKER, brk)
    check(resp is None, "broker config written")
    s, e = commit(d, [noop_rule()])
    check(s == CFG_READY and e == 0, f"COMMIT ok (status {s}, error {e})")
    check(read_block(d, 0) == g0, "block 0 NOT written in this Deploy: keeps its config (unlike FB)")
    for i, g in ((1, g1), (2, g2), (3, g3)):
        got = read_block(d, i)
        check(got == g, f"block {i} reads back byte-exact {diff_regs(got, g) or ''}")
    got = read_broker(d)
    check(broker_cfg_equal(got, brk), f"broker config reads back {diff_regs(got[:68], brk[:68]) or ''}")
    check(all(r == 0 for r in got[68:84]), "password reads back 0")
    check(regs_str(got[4:36]) == "test.invalid" and regs_str(got[52:68]) == "user1", "host/user text round-trips")

    # ------------------------------------------------------------------ 4
    print("\n4. FC06 keeps the other fields of the block")
    resp = d.fc06(REG_TEL + 1 * BLOCK_REGS + 3, 0x1234)   # block 1, interval low word
    check(not resp.isError(), "FC06 on one register accepted")
    s, e = commit(d, [noop_rule()])
    got = read_block(d, 1)
    exp = list(g1)
    exp[3] = 0x1234
    check(s == CFG_READY and got == exp, f"only that register changed {diff_regs(got, exp) or ''}")
    g1 = exp

    # ------------------------------------------------------------------ 5
    print("\n5. COMMIT rejections (whole draft rejected, nothing applied)")
    nan_bits = 0x7FC00000
    neg_bits = 0xBF800000
    bad = [
        ("MODE=3", 2, make_block(True, ctrl_raw=0x0001 | (3 << 1), tags=[va], topic=base + "/x")),
        ("PAYLOAD=3", 2, make_block(True, ctrl_raw=0x0001 | (3 << 6), tags=[va], topic=base + "/x")),
        ("QOS=3", 2, make_block(True, ctrl_raw=0x0001 | (3 << 3), tags=[va], topic=base + "/x")),
        ("tag_count=0", 2, make_block(True, raw_tags=[0xFFFF] * 8, tag_count=0, topic=base + "/x")),
        ("tag_count=9", 2, make_block(True, raw_tags=[va] * 8, tag_count=9, topic=base + "/x")),
        ("bound tag >= MAX_TAGS", 2, make_block(True, tags=[0x7000], topic=base + "/x")),
        ("unused slot not 0xFFFF", 2, make_block(True, raw_tags=[va, 5] + [0xFFFF] * 6, tag_count=1, topic=base + "/x")),
        ("en_tag_index bad", 2, make_block(True, tags=[va], en_tag=0x7000, topic=base + "/x")),
        ("PERIODIC interval 50 ms", 2, make_block(True, MODE_PERIODIC, interval=50, tags=[va], topic=base + "/x")),
        ("deadband NaN", 2, make_block(True, MODE_COV, tags=[va], topic=base + "/x", deadband_bits=nan_bits)),
        ("deadband negative", 2, make_block(True, MODE_COV, tags=[va], topic=base + "/x", deadband_bits=neg_bits)),
        ("topic empty", 2, make_block(True, tags=[va], topic="")),
        ("topic with '+'", 2, make_block(True, tags=[va], topic="a/+/b")),
        ("topic with '#'", 2, make_block(True, tags=[va], topic="a/#")),
        ("topic starts with '/'", 2, make_block(True, tags=[va], topic="/abc")),
        ("topic control char", 2, make_block(True, tags=[va], topic_bytes=b"ab\x07cd")),
        ("template unknown token", 2, make_block(True, payload=PAY_CUSTOM, tags=[va], topic=base + "/x", template='{"a":{foo}}')),
        ("template {val2} with 1 tag", 2, make_block(True, payload=PAY_CUSTOM, tags=[va], topic=base + "/x", template='{"a":{val2}}')),
        ("template token not closed", 2, make_block(True, payload=PAY_CUSTOM, tags=[va], topic=base + "/x", template='{"a":{val1')),
        ("template {val1:4} decimals", 2, make_block(True, payload=PAY_CUSTOM, tags=[va], topic=base + "/x", template='{"a":{val1:4}}')),
    ]
    bad_brokers = [
        ("broker port 0", make_broker(True, port=0, host="h.example", client_id="c")),
        ("broker use_tls=1 (no TLS in this build)", make_broker(True, tls=True, port=8883, host="h.example", client_id="c")),
        ("broker version 2", make_broker(True, version=2, host="h.example", client_id="c")),
    ]
    before = [read_block(d, i) for i in range(TEL_BLOCKS)]
    before_b = read_broker(d)[:BK_STATUS_OFF]
    n_before = rule_count(d)
    toggle = list(before[0])
    toggle[0] ^= 0x0020          # flip retain of block 0: a GOOD change riding with the bad one
    two_rules = [noop_rule(), noop_rule()]

    def expect_reject(name, writes):
        for addr, regs in writes:
            d.write_regs(addr, regs)
        s, e = commit(d, two_rules)
        same = all(read_block(d, i) == before[i] for i in range(TEL_BLOCKS))
        same = same and read_broker(d)[:BK_STATUS_OFF] == before_b
        check(s != CFG_READY and e == ERR_INVALID_PARAMETER,
              f"{name}: rejected (status {s}, error {e}; expect error 2)")
        check(same and rule_count(d) == n_before,
              f"{name}: nothing applied (blocks, broker and rule count unchanged)")

    for name, idx, regs in bad:
        expect_reject(name, [(REG_TEL, toggle), (REG_TEL + idx * BLOCK_REGS, regs)])
    for name, regs in bad_brokers:
        expect_reject(name, [(REG_TEL, toggle), (REG_BROKER, regs)])

    print("  -- draft dropped after a failed COMMIT")
    d.write_regs(REG_TEL + 2 * BLOCK_REGS, bad[0][2])
    commit(d, two_rules)                                   # rejected, drops the draft
    s, e = commit(d, [noop_rule()])                        # no MQTT write this time
    check(s == CFG_READY and all(read_block(d, i) == before[i] for i in range(TEL_BLOCKS)),
          "next Deploy without MQTT writes: old draft is gone, config unchanged")

    print("  -- a DISABLED block is not content-checked")
    junk = make_block(False, topic_bytes=b"/#+\x07", tag_count=0, raw_tags=[7] * 8)
    d.write_regs(REG_TEL + 3 * BLOCK_REGS, junk)
    s, e = commit(d, [noop_rule()])
    check(s == CFG_READY and read_block(d, 3) == junk, "disabled block with junk fields committed as written")
    d.write_regs(REG_TEL + 3 * BLOCK_REGS, g3)             # restore
    s, e = commit(d, [noop_rule()])
    check(s == CFG_READY and read_block(d, 3) == g3, "block 3 restored")

    # ------------------------------------------------------------------ 6
    print("\n6. Address limits and the read-only status area")
    resp = d.fc16(REG_TEL + TEL_BLOCKS * BLOCK_REGS - 1, [0, 0])
    check(resp.isError() and exc_code(resp) == EXC_ADDR, f"FC16 running past 0x0D7F -> exception 2 ({exc_code(resp)})")
    resp = d.fc16(REG_BROKER + BROKER_REGS - 1, [0, 0])
    check(resp.isError() and exc_code(resp) == EXC_ADDR, f"FC16 running past 0x0E5F -> exception 2 ({exc_code(resp)})")
    resp = d.fc16(REG_BSTATUS, [0xFFFF] * 12)
    check(not resp.isError(), "FC16 over the read-only status 0x0E54..0x0E5F accepted")
    st = broker_status(d)
    check(st["flags"] != 0xFFFF and st["reserved"] == [0] * 5 and st["last_err"] <= 4,
          f"...and ignored (flags=0x{st['flags']:04X}, reserved {st['reserved']}, last_err {st['last_err']})")
    s, e = commit(d, [noop_rule()])
    check(s == CFG_READY and broker_cfg_equal(read_broker(d), brk),
          "the ignored status write left the broker config untouched after COMMIT")

    # ------------------------------------------------------------------ 7
    print("\n7. Status area sanity")
    a = broker_status(d)
    time.sleep(1.0)
    b = broker_status(d)
    check(b["tx_msgs"] >= a["tx_msgs"] and b["tx_bytes"] >= a["tx_bytes"], "tx counters never go down")
    info(f"flags=0x{b['flags']:04X} latency={b['latency']} ms tx_msgs={b['tx_msgs']} "
         f"tx_bytes={b['tx_bytes']} (host accepted, not broker acks)")

    full = {"blocks": [g0, g1, g2, g3], "broker": brk}

    def same_config(label):
        ok = all(read_block(d, i) == full["blocks"][i] for i in range(TEL_BLOCKS))
        ok = ok and broker_cfg_equal(read_broker(d), full["broker"])
        return check(ok, label)

    def config_cleared(label):
        ok = all(read_block(d, i)[0] & 1 == 0 for i in range(TEL_BLOCKS)) and read_broker(d)[0] & 1 == 0
        return check(ok, label)

    # ------------------------------------------------------------------ 8
    if args.reboot:
        print("\n8. Persistence and clear commands (--reboot)")
        same_config("config in RAM before REBOOT")
        d2 = d.reboot(args)
        if d2 is None:
            check(False, "board came back after REBOOT")
        else:
            d = d2
            same_config("config survives REBOOT (Flash record: rules + FB + MQTT, one CRC)")
            check(rule_count(d) == 1, "rule table survives too")
            s, e = run_syscmd(d, SYS_CLEAR_RETAIN)
            check(s == ST_DONE, f"CLEAR_RETAIN result {s}/{e}")
            same_config("CLEAR_RETAIN does not touch MQTT")
            s, e = run_syscmd(d, SYS_CLEAR_RULES)
            check(s == ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
            config_cleared("CLEAR_RULES clears MQTT (blocks and broker)")
            d2 = d.reboot(args)
            if d2 is None:
                check(False, "board came back after REBOOT")
            else:
                d = d2
                config_cleared("...and it stays cleared after REBOOT")
                check(rule_count(d) == 0, "rule table empty after REBOOT")
                for i, g in enumerate(full["blocks"]):
                    d.write_regs(REG_TEL + i * BLOCK_REGS, g)
                d.write_regs(REG_BROKER, full["broker"])
                s, e = commit(d, [noop_rule()])
                same_config("config deployed again")
                s, e = run_syscmd(d, SYS_FACTORY)
                check(s == ST_DONE and e == 0, f"FACTORY_RESET result {s}/{e}")
                config_cleared("FACTORY_RESET clears MQTT")
                d2 = d.reboot(args)
                if d2 is not None:
                    d = d2
                    config_cleared("...also after REBOOT")
    else:
        print("\n8. Persistence skipped (use --reboot)")

    # ------------------------------------------------------------------ 9
    if args.broker:
        d = run_broker(d, args, va, vb, base)

    # ------------------------------------------------------------------ 10
    print("\n10. Cleanup: CLEAR_RULES (also removes the test broker and the password from Flash)")
    s, e = run_syscmd(d, SYS_CLEAR_RULES)
    check(s == ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
    config_cleared("MQTT config cleared; the gateway uses its own broker again")


# ----------------------------------------------------------- publish (--broker)
class Sub:
    """paho subscriber that records (time, topic, payload, qos, retain)."""

    def __init__(self, host, port, user, pw, topics, qos=2):
        import paho.mqtt.client as mqtt
        self.q = queue.Queue()
        self.topics = topics
        cid = "splc_test_sub_%04x" % random.randrange(0x10000)
        try:
            self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
        except AttributeError:                       # paho 1.x
            self.c = mqtt.Client(client_id=cid)
        if user:
            self.c.username_pw_set(user, pw)
        self.qos = qos
        self.ready = False

        def on_connect(client, userdata, *a):
            for t in self.topics:
                client.subscribe(t, self.qos)
            self.ready = True

        def on_message(client, userdata, msg):
            self.q.put((time.time(), msg.topic, msg.payload.decode("utf-8", "replace"),
                        msg.qos, bool(msg.retain)))

        self.c.on_connect = on_connect
        self.c.on_message = on_message
        self.c.connect(host, port, 30)
        self.c.loop_start()
        t0 = time.time()
        while not self.ready and time.time() - t0 < 5.0:
            time.sleep(0.05)
        time.sleep(0.5)

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                return out

    def collect(self, seconds):
        time.sleep(seconds)
        return self.drain()

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()


def wait_connected(d, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if broker_status(d)["flags"] & 1:
                return True
        except RuntimeError:
            pass
        time.sleep(1.0)
    return False


def by_topic(msgs, topic):
    return [m for m in msgs if m[1] == topic]


def run_broker(d, args, va, vb, base):
    print("\n9. Real publishing through the gateway (--broker)")
    try:
        import paho.mqtt.client  # noqa: F401
    except ImportError:
        check(False, "paho-mqtt installed (pip install paho-mqtt)")
        return d
    host, _, port = args.broker.partition(":")
    port = int(port) if port else 1883
    cid1 = ("splc_t1_%04x" % random.randrange(0x10000))
    cid2 = ("splc_t2_%04x" % random.randrange(0x10000))
    topics = [f"{base}/b{i}" for i in range(4)]

    sub = Sub(host, port, args.mqtt_user, args.mqtt_pass, [base + "/#"])
    check(sub.ready, f"test subscriber connected to {host}:{port} (PC -> broker)")
    if not sub.ready:
        return d

    # va is bumped by 1 every 200 ms (5/s) by an INTERVAL rule.
    inc_rule = encode_rule(TRG_INTERVAL, OP_NONE, ACT_INC, 0, va, for_ms=200, action_param=1)
    b0 = make_block(True, MODE_PERIODIC, qos=1, interval=2000, tags=[va], topic=topics[0])
    b1 = make_block(True, MODE_COV, qos=0, payload=PAY_RAW, deadband=5.0, tags=[va], topic=topics[1])
    b2 = make_block(True, MODE_ON_CHANGE, qos=1, payload=PAY_COMPACT, tags=[va], topic=topics[2])
    tmpl = '{"a":{val1},"b":{val2},"ts":{ts}}'
    # reserved_header: tag1 -> 1 decimal (bits 0-1 = 1), tag2 -> 2 decimals (bits 2-3 = 2)
    b3 = make_block(True, MODE_PERIODIC, qos=0, retain=True, payload=PAY_CUSTOM, interval=3000,
                    tags=[va, vb], topic=topics[3], template=tmpl, reserved=0x0009)

    def deploy(client_id):
        brk = make_broker(True, port=port, keepalive=30, host=host, client_id=client_id,
                          user=args.mqtt_user or "", password=args.mqtt_pass or "")
        for i, b in enumerate((b0, b1, b2, b3)):
            d.write_regs(REG_TEL + i * BLOCK_REGS, b)
        d.write_regs(REG_BROKER, brk)
        return commit(d, [inc_rule])

    s, e = deploy(cid1)
    check(s == CFG_READY and e == 0, f"Deploy with the test broker as App broker (status {s}, error {e})")
    if s != CFG_READY:
        sub.close()
        return d
    st0 = broker_status(d)
    ok = wait_connected(d, 45.0)
    check(ok, "gateway reports CONNECTED to the test broker (status flags bit 0); "
              "the library only publishes once the connection matches the config generation")
    if not ok:
        info("not connected: broker reachable FROM THE GATEWAY at that address? credentials? "
             f"last_err={broker_status(d)['last_err']}")
        sub.close()
        return d

    print("  collecting 12 s of traffic ...")
    tx_a = broker_status(d)["tx_msgs"]
    msgs = sub.collect(12.0)
    tx_b = broker_status(d)["tx_msgs"]
    info("messages per topic: " + ", ".join(f"b{i}={len(by_topic(msgs, t))}" for i, t in enumerate(topics)))

    m0, m1, m2, m3 = (by_topic(msgs, t) for t in topics)
    # b0 PERIODIC 2 s compact JSON, QoS 1
    check(len(m0) >= 4, f"b0 PERIODIC 2000 ms: {len(m0)} messages in 12 s (expect ~6, baseline first)")
    try:
        js = [json.loads(m[2]) for m in m0]
        check(all(set(j) == {"tag", "val", "ts"} and j["tag"] == va and isinstance(j["val"], int) for j in js),
              f"b0 compact JSON has tag={va}, integer val, ts (e.g. {m0[0][2]})")
        gaps = sorted(m0[i + 1][0] - m0[i][0] for i in range(len(m0) - 1))
        med = gaps[len(gaps) // 2] if gaps else 0
        check(1.4 <= med <= 3.0, f"b0 median gap {med:.2f} s (expect ~2 s)")
        vals = [j["val"] for j in js]
        check(vals == sorted(vals) and vals[-1] > vals[0], "b0 values grow (the rule is counting)")
    except Exception as ex:
        check(False, f"b0 payload parse: {ex}")
    check(all(m[3] == 1 for m in m0), "b0 delivered with QoS 1")
    # b1 COV deadband 5, RAW
    check(len(m1) >= 4, f"b1 COV deadband 5: {len(m1)} messages (expect ~1 per second)")
    try:
        v1 = [int(m[2]) for m in m1]
        check(all(v1[i + 1] - v1[i] >= 5 for i in range(len(v1) - 1)),
              f"b1 RAW payload = plain integer, consecutive values >= 5 apart ({v1[:6]}...)")
    except Exception as ex:
        check(False, f"b1 RAW payload is not an integer string: {ex}")
    check(all(m[3] == 0 for m in m1), "b1 delivered with QoS 0")
    # b2 ON_CHANGE
    check(len(m2) >= 15, f"b2 ON_CHANGE: {len(m2)} messages (expect ~5 per second)")
    try:
        v2 = [json.loads(m[2])["val"] for m in m2]
        check(v2 == sorted(v2) and len(set(v2)) == len(v2), "b2 every message carries a NEW value, in order")
    except Exception as ex:
        check(False, f"b2 payload parse: {ex}")
    check(len(m2) > len(m1) > 0 and len(m1) >= len(m0) - 1,
          "rate order ON_CHANGE > COV >= PERIODIC")
    # b3 Custom + decimals + retain
    check(len(m3) >= 2, f"b3 PERIODIC 3000 ms Custom: {len(m3)} messages")
    if m3:
        txt = m3[-1][2]
        ma = re.search(r'"a":(-?\d+\.\d)[,}]', txt)
        mb = re.search(r'"b":(-?\d+\.\d\d)[,}]', txt)
        check(bool(ma) and bool(mb), f"b3 decimals follow reserved_header (a: 1 decimal, b: 2) -> {txt}")
        try:
            json.loads(txt)
            check(True, "b3 payload is valid JSON ({device} and {ts} filled)")
        except Exception as ex:
            check(False, f"b3 payload JSON: {ex}: {txt}")
        info("{ts} = 0 is expected on a gateway with no clock owner yet (decision 23 / Q7)")
        check(all(m[3] == 0 for m in m3), "b3 delivered with QoS 0")
    # tx counter
    got = len(msgs)
    delta = tx_b - tx_a
    check(delta >= got - 3, f"tx_message_count +{delta} >= messages received {got} "
                            "(counts publishes the host ACCEPTED)")

    # retain: a late subscriber must get b3 (retain=1) and not b0 (retain=0)
    late = Sub(host, port, args.mqtt_user, args.mqtt_pass, [base + "/#"])
    lm = late.collect(2.0)
    late.close()
    check(any(m[1] == topics[3] and m[4] for m in lm), "late subscriber receives b3 as a RETAINED message")
    check(not any(m[1] == topics[0] and m[4] for m in lm), "...and no RETAINED message from b0 (retain=0)")

    # Q4: no publish while DIAG holds the engine
    print("  -- DIAG holds the engine (Q4: no publish)")
    sub.drain()
    d.fc06(REG_DIAG, CMD_ENTER)
    t_enter = time.time()
    held = []
    while time.time() - t_enter < 5.0:
        time.sleep(1.0)
        d.fc06(REG_DIAG, CMD_HEARTBEAT)
    held = [m for m in sub.drain() if m[0] > t_enter + 0.8]
    check(len(held) == 0, f"no message published while DIAG holds the engine ({len(held)} seen)")
    d.fc06(REG_DIAG, CMD_EXIT)
    after = sub.collect(6.0)
    check(len(after) > 0, f"publishing resumes after EXIT_DIAG ({len(after)} messages in 6 s)")

    # generation change: new client_id -> host reconnects, every block fires a baseline again
    print("  -- broker config changes (new client_id): reconnect and baseline")
    sub.drain()
    s, e = deploy(cid2)
    check(s == CFG_READY and e == 0, f"Deploy with a new client_id (status {s}, error {e})")
    ok = wait_connected(d, 60.0)
    check(ok, "gateway reconnected with the new config")
    time.sleep(1.0)
    msgs = sub.collect(8.0)
    seen = [len(by_topic(msgs, t)) for t in topics]
    check(all(n >= 1 for n in seen), f"every block published again after the reconnect (b0..b3 = {seen})")
    sub.close()
    return d


# ------------------------------------------------------------------------ main
def main():
    global _failures
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48,
                   help="registers per FC16 when writing blocks (default 48 = 105-byte frame)")
    p.add_argument("--reboot", action="store_true", help="also check persistence across REBOOT")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    p.add_argument("--broker", default=None, metavar="HOST[:PORT]",
                   help="run the real publish tests against this broker (needs paho-mqtt; the "
                        "gateway must be able to reach it)")
    p.add_argument("--mqtt-user", default=None)
    p.add_argument("--mqtt-pass", default=None)
    args = p.parse_args()
    Dev.chunk = max(1, min(args.chunk, 120))

    from pymodbus.client import ModbusSerialClient
    client = ModbusSerialClient(port=args.port, baudrate=args.baudrate, parity="N",
                                stopbits=1, bytesize=8, timeout=1.0)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    d = Dev(client, args.unit)
    try:
        run(d, args)
    finally:
        try:
            d.c.close()
        except Exception:
            pass
    if Dev.retried:
        print(f"\n(info) {Dev.retried} read attempt(s) needed a retry: check the RS485 bus if this is not 0")
    print("\nALL PASS" if _failures == 0 else f"\n{_failures} CHECK(S) FAILED")
    sys.exit(0 if _failures == 0 else 1)


if __name__ == "__main__":
    main()
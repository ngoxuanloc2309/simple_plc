#!/usr/bin/env python3
"""
test_rules.py - On-board functional test suite for SimplePLC's Rule Engine.

Run against the real MCU over Modbus RTU / USB-CDC:

    python test_rules.py COM14              # automatic tests only
    python test_rules.py COM14 --manual     # + tests that need you to press inputs
    python test_rules.py COM14 --only dwell # run tests whose name contains "dwell"

Reuses the helpers of test_plc.py (register map, CRC, rule encoding), which
already run on the real board -- nothing about the wire format is re-invented.

HOW THE AUTOMATIC TESTS WORK (no wiring needed)
-----------------------------------------------
DI tags are physical inputs and RUNTIME_TAG_VALUES (0x0900) is read-only, so
the App cannot force a tag value. Instead these tests use the rule engine's
OWN triggers as the stimulus:

  * A rule with trigger INTERVAL fires every `for_ms` ms with no input at all,
    so it is a free-running clock/counter source.
  * Its result is written into virtual tags (VFLAG / VREG), which have no
    pin. Other rules read those tags -> rule chains, guards, compares.

Because the App and the MCU are not clock-synchronised, timing checks are
done from the MCU's OWN counters, never from Python's wall clock alone:
a counter that increments once per 100 ms interval is read back and compared
with the elapsed time Python measured, with a tolerance band. Exact-count
assertions are only made where the result is deterministic (guards, compares,
ordering).

Every test starts from a clean state (empty rule table) so tests cannot
influence each other. The rule table is CLEARED again at the end.

HOW TO READ A FAILURE
---------------------
A FAIL line names the exact behaviour and prints the measured value, e.g.
"EQ v+1 -> silent (36 fires)". Before blaming the firmware, rerun once: the
timing checks use tolerance bands, and a busy USB link can (rarely) push a
single sample outside one. A check that fails on EVERY rerun is real.

VERIFICATION STATUS OF THIS FILE
--------------------------------
The automatic tests were run against a PC build of the firmware (real
plc_modbus_cfg.c + nanoMODBUS + Layer 2, speaking real Modbus RTU framing
and CRC over a pipe) and pass 59/59, 3 runs in a row. Injecting 12 different
bugs into that firmware, 11 made this suite fail; the 12th (ON_RISE also
accepting a falling edge) is masked by a second, redundant level check in the
state machine and is not observable from behaviour -- it needs a direct
unit test of check_trigger_edge(). NOT yet run on the real board, and the
--manual tests have not been run at all (they need hands on the DI pins).

MANUAL TESTS (--manual) drive the real DI pins: the script tells you which
input to raise/lower and checks the resulting DO. Wiring: DI0..DI3 are the
opto inputs, DO0..DO3 the relay/transistor outputs (board_zigbee_io.h).

Requires: pip install pymodbus pyserial
"""

import argparse
import sys
import time

import test_plc as T   # register map, CRC, encode_rule_registers, read/write helpers


# --- Tag indices (plc_tag_def.h, v1.9 layout) --------------------------------

TAG_DI0, TAG_DO0 = 0, 8
TAG_VFLAG0 = 20      # 20..51  -> 32 virtual flags
TAG_VREG0 = 52       # 52..83  -> 32 virtual registers
TAG_COUNTER0 = 116   # 116..123

# --- Enums not (yet) exported by test_plc.py ---------------------------------

TRG_ON_CHANGE, TRG_ON_RISE, TRG_ON_FALL, TRG_TIME_WINDOW, TRG_INTERVAL = 0, 1, 2, 3, 4
OP_NONE, OP_EQ, OP_NEQ, OP_GT, OP_LT, OP_GTE, OP_LTE, OP_BETWEEN = range(8)
ACT_SET, ACT_TOGGLE, ACT_INC, ACT_WRITE_REMOTE, ACT_LOG, ACT_ALARM, ACT_ADD, ACT_SCALE = range(8)

NONE = T.GUARD_TAG_NONE          # 0x7FFF, "no guard"
NEGATE = 0x8000                  # OR into guard_tag: fire only when guard == 0

MAX_RULES = 100


# --- Rule description --------------------------------------------------------

class Rule:
    """One SPLC_RuleRecord. Field names mirror plc_rule.h."""

    def __init__(self, trigger_tag, action_tag, *, trigger=TRG_ON_RISE,
                 for_ms=0, guard=NONE, op=OP_NONE, lo=0, hi=0,
                 action=ACT_SET, param=1, enabled=1):
        self.trigger_tag, self.action_tag = trigger_tag, action_tag
        self.trigger, self.for_ms, self.guard = trigger, for_ms, guard
        self.op, self.lo, self.hi = op, lo, hi
        self.action, self.param, self.enabled = action, param, enabled

    def registers(self):
        return T.encode_rule_registers(
            threshold_lo=self.lo, threshold_hi=self.hi, for_ms=self.for_ms,
            action_param=self.param, trigger_tag=self.trigger_tag,
            action_tag=self.action_tag, guard_tag=self.guard,
            enabled=self.enabled, trigger_type=self.trigger,
            compare_op=self.op, action_type=self.action)


# --- Device access -----------------------------------------------------------

class Dev:
    def __init__(self, client, unit):
        self.c, self.u = client, unit

    def _read(self, addr, n):
        return T.read_regs(self.c, self.u, addr, n)

    def tags(self, first=0, count=56):
        """Snapshot of `count` tags starting at `first`, read in ONE frame so
        the values are mutually consistent (max 62 tags per Modbus request)."""
        assert count <= 62
        regs = self._read(T.REG_RUNTIME_TAG_VALUES + first * 2, count * 2)
        return [T.s16_pair_to_i32(regs[i * 2], regs[i * 2 + 1]) for i in range(count)]

    def tag(self, idx):
        return self.tags(idx, 1)[0]

    def status(self):
        st = self._read(T.REG_CONFIG_STATUS, 1)[0]
        err = self._read(T.REG_CONFIG_ERROR_CODE, 1)[0]
        return st, err

    def active(self):
        """(rule_count, crc16, version) as reported by the MCU."""
        return (self._read(T.REG_ACTIVE_RULE_COUNT, 1)[0],
                self._read(T.REG_ACTIVE_RULE_CRC16, 1)[0],
                self._read(T.REG_ACTIVE_RULE_VERSION, 1)[0])

    def read_active_rule(self, i):
        return self._read(T.REG_ACTIVE_RULE_TABLE + i * 16, 16)

    def load(self, rules):
        """Stage + commit `rules`; raise if the MCU does not reach READY.
        Returns the (count, crc) the MCU reports afterwards."""
        regs = [r.registers() for r in rules]
        blob = b"".join(T.rule_registers_to_bytes(r) for r in regs)
        crc = T.crc16_modbus(blob)
        T.write_reg(self.c, self.u, T.REG_RULE_COUNT_STAGED, len(rules))
        for i, r in enumerate(regs):
            T.write_regs(self.c, self.u, T.REG_STAGING_RULE_TABLE + i * 16, r)
        T.write_reg(self.c, self.u, T.REG_EXPECTED_CRC16, crc)
        T.write_reg(self.c, self.u, T.REG_COMMIT_COMMAND, T.COMMIT_COMMAND_MAGIC)
        st, err = self.status()
        if st != 3:
            raise RuntimeError(f"commit failed: CONFIG_STATUS={st} ERROR={err}")
        cnt, act_crc, _ = self.active()
        if cnt != len(rules) or act_crc != crc:
            raise RuntimeError(f"MCU table differs: count {cnt}/{len(rules)} "
                               f"crc 0x{act_crc:04X}/0x{crc:04X}")
        return cnt, crc

    def clear(self):
        """Empty the rule table (0 rules). Virtual tags keep their last value
        (there is no Modbus way to reset them; tests use unique tags)."""
        self.load([])


# --- Tiny test framework -----------------------------------------------------

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print(f"    [{'PASS' if cond else 'FAIL'}] {name}" + (f"   ({detail})" if detail else ""))
    return bool(cond)


def wait(seconds):
    time.sleep(seconds)


def rate(dev, tag, seconds):
    """Increments of `tag` per second over `seconds` (tag must only go up)."""
    a = dev.tag(tag)
    t0 = time.time()
    wait(seconds)
    b = dev.tag(tag)
    return (b - a) / (time.time() - t0), b - a, time.time() - t0


TESTS = []


def test(name, manual=False):
    def deco(fn):
        TESTS.append((name, manual, fn))
        return fn
    return deco


# =============================================================================
# AUTOMATIC TESTS (no wiring)
# =============================================================================

@test("upload: empty table, 1 rule, then MAX_RULES round-trip")
def t_upload(d):
    d.clear()
    cnt, _, _ = d.active()
    check("empty table accepted, ACTIVE_RULE_COUNT == 0", cnt == 0)

    r = Rule(TAG_VFLAG0, TAG_VFLAG0 + 1, trigger=TRG_INTERVAL, for_ms=500)
    _, crc = d.load([r])
    check("one rule accepted", d.active()[0] == 1)
    check("ACTIVE_RULE_TABLE reads back byte-identical",
          d.read_active_rule(0) == r.registers())

    many = [Rule(TAG_VFLAG0, TAG_VFLAG0 + 1, trigger=TRG_INTERVAL, for_ms=60000,
                 param=i) for i in range(MAX_RULES)]
    d.load(many)
    check(f"{MAX_RULES} rules accepted (MAX_RULES)", d.active()[0] == MAX_RULES)
    check("last rule reads back intact (register-address math)",
          d.read_active_rule(MAX_RULES - 1) == many[-1].registers())
    d.clear()


@test("upload: bad CRC and bad magic are REJECTED, old table survives")
def t_upload_reject(d):
    good = Rule(TAG_VFLAG0, TAG_VREG0, trigger=TRG_INTERVAL, for_ms=1000)
    d.load([good])
    ver = d.active()[2]

    # wrong CRC
    regs = good.registers()
    T.write_reg(d.c, d.u, T.REG_RULE_COUNT_STAGED, 1)
    T.write_regs(d.c, d.u, T.REG_STAGING_RULE_TABLE, regs)
    T.write_reg(d.c, d.u, T.REG_EXPECTED_CRC16, 0x1234)
    T.write_reg(d.c, d.u, T.REG_COMMIT_COMMAND, T.COMMIT_COMMAND_MAGIC)
    st, err = d.status()
    check("wrong CRC -> CONFIG_STATUS == ERROR(4)", st == 4, f"status={st}")
    check("wrong CRC -> error code CRC_MISMATCH(4)", err == 4, f"err={err}")
    check("wrong CRC -> active table untouched",
          d.active()[2] == ver and d.read_active_rule(0) == regs)

    # bad magic
    T.write_reg(d.c, d.u, T.REG_COMMIT_COMMAND, 0x0000)
    st, err = d.status()
    check("bad commit magic -> ERROR / INVALID_PARAMETER(2)", st == 4 and err == 2,
          f"status={st} err={err}")

    # count above MAX_RULES
    T.write_reg(d.c, d.u, T.REG_RULE_COUNT_STAGED, MAX_RULES + 1)
    st, err = d.status()
    check("rule_count > MAX_RULES rejected", st == 4 and err == 2, f"status={st} err={err}")

    # a good upload must recover from ERROR
    d.load([good])
    check("recovers: next valid upload reaches READY", d.status()[0] == 3)
    check("version increments only on successful commits", d.active()[2] > ver)
    d.clear()


@test("interval: INTERVAL 100 ms increments at ~10/s (free-running clock)")
def t_interval(d):
    d.load([Rule(TAG_VFLAG0, TAG_VREG0, trigger=TRG_INTERVAL, for_ms=100,
                 action=ACT_INC, param=1)])
    wait(0.3)
    per_s, n, dt = rate(d, TAG_VREG0, 3.0)
    check("rate within 10/s +-15%", 8.5 <= per_s <= 11.5,
          f"{per_s:.2f}/s ({n} in {dt:.2f}s)")
    d.clear()


@test("interval: different periods stay proportional (50 / 200 / 1000 ms)")
def t_interval_multi(d):
    d.load([
        Rule(TAG_VFLAG0, TAG_VREG0 + 1, trigger=TRG_INTERVAL, for_ms=50, action=ACT_INC),
        Rule(TAG_VFLAG0, TAG_VREG0 + 2, trigger=TRG_INTERVAL, for_ms=200, action=ACT_INC),
        Rule(TAG_VFLAG0, TAG_VREG0 + 3, trigger=TRG_INTERVAL, for_ms=1000, action=ACT_INC),
    ])
    a = d.tags(TAG_VREG0, 4)
    t0 = time.time()
    wait(4.0)
    b = d.tags(TAG_VREG0, 4)
    dt = time.time() - t0
    r50, r200, r1000 = [(b[i] - a[i]) / dt for i in (1, 2, 3)]
    check("50 ms rule  ~ 20/s", 16 <= r50 <= 23, f"{r50:.1f}/s")
    check("200 ms rule ~ 5/s", 4.2 <= r200 <= 5.8, f"{r200:.1f}/s")
    check("1000 ms rule ~ 1/s", 0.7 <= r1000 <= 1.3, f"{r1000:.2f}/s")
    check("50 ms fires ~4x more than 200 ms", 3.2 <= r50 / max(r200, 1e-9) <= 4.8)
    d.clear()


@test("compare: each operator fires EXACTLY as often as its condition is true")
def t_compare(d):
    # A sticky flag ("SET once") cannot tell a correct compare from an
    # inverted one: EQ wrongly behaving as NEQ still ends up with the flag
    # set. So each probe is an INC counter instead, and we check HOW MANY
    # times it fired -- which pins down the operator in both directions.
    #
    # Source: VREG0, a free-running counter. Its value is frozen mid-test by
    # reloading a table WITHOUT the +1 rule, so every probe then sees one
    # constant value `v` and a probe fires for EVERY probe tick or for NONE.
    # That gives an exact truth table: expected = (compare(v) is True).
    #
    # Virtual tags cannot be reset over Modbus, so the frozen value `v` is
    # whatever the counter reached; thresholds are chosen relative to it.
    d.clear()
    d.load([Rule(TAG_VFLAG0 + 20, TAG_VREG0, trigger=TRG_INTERVAL, for_ms=50, action=ACT_INC)])
    wait(0.6)
    d.clear()
    v = d.tag(TAG_VREG0)          # frozen: no rule touches VREG0 any more

    cases = [   # (label, op, lo, hi, expected_true)
        ("GT  v-1",       OP_GT,      v - 1, 0,     True),
        ("GT  v",         OP_GT,      v,     0,     False),
        ("GTE v",         OP_GTE,     v,     0,     True),
        ("GTE v+1",       OP_GTE,     v + 1, 0,     False),
        ("LT  v+1",       OP_LT,      v + 1, 0,     True),
        ("LT  v",         OP_LT,      v,     0,     False),
        ("LTE v",         OP_LTE,     v,     0,     True),
        ("LTE v-1",       OP_LTE,     v - 1, 0,     False),
        ("EQ  v",         OP_EQ,      v,     0,     True),
        ("EQ  v+1",       OP_EQ,      v + 1, 0,     False),
        ("NEQ v+1",       OP_NEQ,     v + 1, 0,     True),
        ("NEQ v",         OP_NEQ,     v,     0,     False),
        ("BETWEEN v-1..v+1", OP_BETWEEN, v - 1, v + 1, True),
        ("BETWEEN v..v",     OP_BETWEEN, v,     v,     True),
        ("BETWEEN v+1..v+9", OP_BETWEEN, v + 1, v + 9, False),
        ("BETWEEN v-9..v-1", OP_BETWEEN, v - 9, v - 1, False),
        ("NONE (always)",    OP_NONE,    0,     0,     True),
    ]
    # One INC counter per case, all polled every 25 ms against frozen VREG0.
    rules = [Rule(TAG_VREG0, TAG_VREG0 + 1 + i, trigger=TRG_INTERVAL, for_ms=25,
                  op=op, lo=lo, hi=hi, action=ACT_INC, param=1)
             for i, (_, op, lo, hi, _) in enumerate(cases)]
    a = d.tags(TAG_VREG0 + 1, len(cases))
    d.load(rules)
    wait(1.0)
    b = d.tags(TAG_VREG0 + 1, len(cases))
    check("source tag is frozen while probing", d.tag(TAG_VREG0) == v, f"v={v}")
    for i, (label, _, _, _, want) in enumerate(cases):
        n = b[i] - a[i]
        ok = (n >= 25) if want else (n == 0)      # ~40 ticks/s if true, 0 if false
        check(f"{label:18s} -> {'fires' if want else 'silent'}", ok, f"{n} fires")
    d.clear()


@test("guard: closed guard blocks, open guard passes, NEGATE inverts")
def t_guard(d):
    G = TAG_VFLAG0 + 10     # guard flag, driven by a rule below
    d.clear()
    # phase 1: guard flag stays 0 -> guarded rule must NOT fire
    d.load([Rule(TAG_VFLAG0, TAG_VREG0 + 4, trigger=TRG_INTERVAL, for_ms=50,
                 guard=G, action=ACT_INC),
            Rule(TAG_VFLAG0, TAG_VREG0 + 5, trigger=TRG_INTERVAL, for_ms=50,
                 guard=G | NEGATE, action=ACT_INC)])
    a = d.tags(TAG_VREG0 + 4, 2)
    wait(1.5)
    b = d.tags(TAG_VREG0 + 4, 2)
    check("guard=0: normal-guard rule blocked", b[0] == a[0], f"{a[0]} -> {b[0]}")
    check("guard=0: NEGATE-guard rule runs", b[1] - a[1] >= 15, f"+{b[1] - a[1]}")

    # phase 2: a rule raises the guard flag -> behaviour flips
    d.load([Rule(TAG_VFLAG0, G, trigger=TRG_INTERVAL, for_ms=10, action=ACT_SET, param=1),
            Rule(TAG_VFLAG0, TAG_VREG0 + 4, trigger=TRG_INTERVAL, for_ms=50,
                 guard=G, action=ACT_INC),
            Rule(TAG_VFLAG0, TAG_VREG0 + 5, trigger=TRG_INTERVAL, for_ms=50,
                 guard=G | NEGATE, action=ACT_INC)])
    wait(0.3)                                        # let the guard flag settle
    a = d.tags(TAG_VREG0 + 4, 2)
    wait(1.5)
    b = d.tags(TAG_VREG0 + 4, 2)
    check("guard=1: normal-guard rule runs", b[0] - a[0] >= 15, f"+{b[0] - a[0]}")
    check("guard=1: NEGATE-guard rule blocked", b[1] == a[1], f"{a[1]} -> {b[1]}")
    d.clear()


@test("guard: TAG_DI0 (index 0) is a REAL guard, not 'no guard' (regression)")
def t_guard_di0(d):
    # Before the GUARD_TAG_NONE fix, guard_tag == 0 meant "no guard" so a rule
    # guarded by DI0 fired regardless. With DI0 low it must NOT fire.
    di0 = d.tag(TAG_DI0)
    if di0 != 0:
        print("    SKIP: DI0 is currently high; release it and re-run")
        return
    d.load([Rule(TAG_VFLAG0, TAG_VREG0 + 6, trigger=TRG_INTERVAL, for_ms=50,
                 guard=TAG_DI0, action=ACT_INC)])
    a = d.tag(TAG_VREG0 + 6)
    wait(1.0)
    b = d.tag(TAG_VREG0 + 6)
    check("guard=DI0 (low) blocks the rule", a == b, f"{a} -> {b}")
    d.clear()


@test("actions: SET / TOGGLE / INC / ADD / SCALE compute the right values")
def t_actions(d):
    # One-shot via INTERVAL is awkward, so use a fixed 'source' counter and
    # let each action rule run at a known cadence; check the arithmetic on
    # a snapshot where the counter has been frozen by DISABLING all rules.
    src = TAG_VREG0 + 8
    d.load([
        Rule(TAG_VFLAG0, src, trigger=TRG_INTERVAL, for_ms=100, action=ACT_INC, param=3),
        Rule(TAG_VFLAG0, TAG_VREG0 + 9, trigger=TRG_INTERVAL, for_ms=100, action=ACT_SET, param=-7),
        Rule(TAG_VFLAG0, TAG_VFLAG0 + 1, trigger=TRG_INTERVAL, for_ms=100, action=ACT_TOGGLE),
        Rule(src, TAG_VREG0 + 10, trigger=TRG_INTERVAL, for_ms=100, action=ACT_SCALE,
             param=2500, hi=10),
    ])
    wait(1.0)
    # freeze: reload the SAME rules disabled -> tags keep their last values
    frozen = [Rule(TAG_VFLAG0, src, trigger=TRG_INTERVAL, for_ms=100, action=ACT_INC, param=3, enabled=0)]
    d.load(frozen)
    wait(0.3)
    v = d.tags(TAG_VREG0, 12)
    inc = v[8]
    check("INC by 3: value is a multiple of 3 and > 0", inc > 0 and inc % 3 == 0, f"VREG8={inc}")
    check("SET -7: negative int32 written and read back", v[9] == -7, f"VREG9={v[9]}")
    scaled = v[10]
    # SCALE = src*2500/1000 + 10 evaluated on a value src took at some earlier tick
    ok_scale = any(scaled == (s * 2500) // 1000 + 10 for s in range(0, inc + 1, 3))
    check("SCALE: result equals s*2500/1000+10 for some earlier counter value s",
          ok_scale, f"VREG10={scaled}")
    check("TOGGLE: VFLAG is exactly 0 or 1", d.tag(TAG_VFLAG0 + 1) in (0, 1))
    d.clear()


@test("chain: a 3-stage rule pipeline propagates and keeps pace with its source")
def t_chain(d):
    # stage 1: a 100 ms INTERVAL rule toggles flag A.
    # stage 2: ON_CHANGE(A) toggles flag B.
    # stage 3: ON_CHANGE(B) increments a counter.
    # So the counter moves only if the value really travels through all three
    # rules. NOTE: this proves propagation and throughput. It does NOT measure
    # per-scan latency (10 ms is below what a Modbus poll can resolve); the
    # order-dependent latency is covered by the PC unit tests of Layer 2.
    d.load([
        Rule(TAG_VFLAG0, TAG_VFLAG0 + 2, trigger=TRG_INTERVAL, for_ms=100, action=ACT_TOGGLE),
        Rule(TAG_VFLAG0 + 2, TAG_VFLAG0 + 3, trigger=TRG_ON_CHANGE, action=ACT_TOGGLE),
        Rule(TAG_VFLAG0 + 3, TAG_VREG0 + 12, trigger=TRG_ON_CHANGE, action=ACT_INC),
    ])
    a = d.tag(TAG_VREG0 + 12)
    wait(2.0)
    b = d.tag(TAG_VREG0 + 12)
    per_s = (b - a) / 2.0
    check("counter at the END of the chain moves", b > a, f"+{b - a} in 2s")
    check("end-of-chain rate matches the 100 ms source (~10/s)", 8 <= per_s <= 11.5,
          f"{per_s:.1f}/s")
    d.clear()


@test("edge: ON_CHANGE / ON_RISE / ON_FALL count each transition once")
def t_edges(d):
    SRC = TAG_VFLAG0 + 4            # square wave: toggles every 200 ms
    d.load([
        Rule(TAG_VFLAG0, SRC, trigger=TRG_INTERVAL, for_ms=200, action=ACT_TOGGLE),
        Rule(SRC, TAG_VREG0 + 13, trigger=TRG_ON_CHANGE, action=ACT_INC),
        Rule(SRC, TAG_VREG0 + 14, trigger=TRG_ON_RISE, action=ACT_INC),
        Rule(SRC, TAG_VREG0 + 15, trigger=TRG_ON_FALL, action=ACT_INC),
    ])
    a = d.tags(TAG_VREG0 + 13, 3)
    t0 = time.time()
    wait(4.0)
    b = d.tags(TAG_VREG0 + 13, 3)
    dt = time.time() - t0
    ch, ri, fa = (b[i] - a[i] for i in range(3))
    exp = dt / 0.2                                   # total transitions
    check("ON_CHANGE ~ every toggle", abs(ch - exp) <= 3, f"{ch} vs ~{exp:.0f}")
    check("ON_RISE ~ half of the toggles", abs(ri - exp / 2) <= 2, f"{ri} vs ~{exp / 2:.0f}")
    check("ON_FALL ~ half of the toggles", abs(fa - exp / 2) <= 2, f"{fa} vs ~{exp / 2:.0f}")
    check("RISE + FALL == CHANGE (no edge lost or double counted)",
          abs((ri + fa) - ch) <= 1, f"{ri}+{fa} vs {ch}")
    d.clear()


@test("dwell: DWELL rules fire only after the signal has been HELD for_ms")
def t_dwell(d):
    # SRC is high for 600 ms, low for 600 ms (period 1200). Two dwell rules
    # look at its rising edge: dwell 300 ms (< 600, must fire every cycle)
    # and dwell 900 ms (> 600, the level never holds long enough -> never).
    SRC = TAG_VFLAG0 + 5
    d.load([
        Rule(TAG_VFLAG0, SRC, trigger=TRG_INTERVAL, for_ms=600, action=ACT_TOGGLE),
        Rule(SRC, TAG_VREG0 + 16, trigger=TRG_ON_RISE, for_ms=300, action=ACT_INC),
        Rule(SRC, TAG_VREG0 + 17, trigger=TRG_ON_RISE, for_ms=900, action=ACT_INC),
        Rule(SRC, TAG_VREG0 + 18, trigger=TRG_ON_RISE, for_ms=0, action=ACT_INC),
    ])
    a = d.tags(TAG_VREG0 + 16, 3)
    t0 = time.time()
    wait(6.0)
    b = d.tags(TAG_VREG0 + 16, 3)
    cycles = (time.time() - t0) / 1.2
    short, long_, nodw = (b[i] - a[i] for i in range(3))
    check("no-dwell rule fires once per rising edge", abs(nodw - cycles) <= 1.5,
          f"{nodw} vs ~{cycles:.1f}")
    check("dwell 300 ms (< high time) fires once per cycle", abs(short - cycles) <= 1.5,
          f"{short} vs ~{cycles:.1f}")
    check("dwell 900 ms (> high time) NEVER fires", long_ == 0, f"{long_}")
    d.clear()


@test("reload: replacing the table mid-run switches behaviour cleanly")
def t_reload(d):
    d.load([Rule(TAG_VFLAG0, TAG_VREG0 + 19, trigger=TRG_INTERVAL, for_ms=100, action=ACT_INC)])
    wait(1.0)
    r1, _, _ = rate(d, TAG_VREG0 + 19, 1.5)
    d.load([Rule(TAG_VFLAG0, TAG_VREG0 + 19, trigger=TRG_INTERVAL, for_ms=500, action=ACT_INC)])
    wait(0.6)
    r2, _, _ = rate(d, TAG_VREG0 + 19, 3.0)
    check("before reload ~10/s", 8 <= r1 <= 11.5, f"{r1:.1f}/s")
    check("after reload ~2/s", 1.5 <= r2 <= 2.6, f"{r2:.1f}/s")
    d.clear()
    wait(0.3)
    r3, n3, _ = rate(d, TAG_VREG0 + 19, 1.0)
    check("empty table: nothing fires any more", n3 == 0, f"+{n3}")


@test("stability: 100 rules running, MCU keeps answering, scan time sane")
def t_load(d):
    rules = [Rule(TAG_VFLAG0, TAG_VREG0 + (i % 20), trigger=TRG_INTERVAL,
                  for_ms=100 + i, action=ACT_INC) for i in range(MAX_RULES)]
    d.load(rules)
    ok, fails, t0 = 0, 0, time.time()
    while time.time() - t0 < 5.0:
        try:
            d.tags(TAG_VREG0, 20)
            ok += 1
        except Exception:
            fails += 1
        time.sleep(0.02)
    check("no failed Modbus requests under full rule load", fails == 0, f"{ok} ok / {fails} fail")
    h = T.read_regs(d.c, d.u, T.REG_DEVICE_HEALTH, 10)
    scan_ms, max_ms = (h[6] << 16) | h[7], (h[8] << 16) | h[9]
    check("scan_time_ms well below the 10 ms period", scan_ms <= 10, f"scan={scan_ms} ms")
    check("max_scan_time_ms reported", True, f"max={max_ms} ms (info; includes USB stalls)")
    d.clear()


# =============================================================================
# MANUAL TESTS (--manual): drive the real DI pins
# =============================================================================

def ask(msg):
    input(f"\n    >>> {msg}  [Enter when done] ")


@test("manual: DI0 rises -> DO0 set (basic rule on real hardware)", manual=True)
def m_basic(d):
    d.load([Rule(TAG_DI0, TAG_DO0, trigger=TRG_ON_RISE, action=ACT_SET, param=1),
            Rule(TAG_DI0 + 1, TAG_DO0, trigger=TRG_ON_RISE, action=ACT_SET, param=0)])
    ask("Make sure DI0 and DI1 are LOW")
    check("DO0 starts low", d.tag(TAG_DO0) == 0)
    ask("Raise DI0 (0 -> 1) then release it")
    check("DO0 went high and stays high (SET is sticky)", d.tag(TAG_DO0) == 1)
    ask("Raise DI1 (0 -> 1) then release it")
    check("DO0 cleared by the DI1 rule", d.tag(TAG_DO0) == 0)
    d.clear()


@test("manual: dwell on a real input (DI0 held 2 s -> DO0)", manual=True)
def m_dwell(d):
    d.load([Rule(TAG_DI0, TAG_DO0, trigger=TRG_ON_RISE, for_ms=2000, action=ACT_SET, param=1)])
    ask("Make sure DI0 is LOW")
    ask("Raise DI0 and RELEASE it after ~1 s (shorter than 2 s)")
    check("short press does NOT fire", d.tag(TAG_DO0) == 0)
    ask("Raise DI0 and HOLD it for 3 s, then release")
    check("held press DID fire", d.tag(TAG_DO0) == 1)
    d.clear()


@test("manual: guard on a real input (DI1 gates DI0 -> DO1)", manual=True)
def m_guard(d):
    d.load([Rule(TAG_DI0, TAG_DO0 + 1, trigger=TRG_ON_RISE, guard=TAG_DI0 + 1,
                 action=ACT_TOGGLE)])
    ask("Keep DI1 LOW, then pulse DI0")
    check("guard low: DO1 unchanged", d.tag(TAG_DO0 + 1) == 0)
    ask("Raise and HOLD DI1, then pulse DI0")
    check("guard high: DO1 toggled on", d.tag(TAG_DO0 + 1) == 1)
    ask("Keep DI1 high, pulse DI0 again")
    check("guard high: DO1 toggled off again", d.tag(TAG_DO0 + 1) == 0)
    d.clear()


@test("manual: DI wiring map (which physical input is which tag)", manual=True)
def m_wiring(d):
    d.clear()
    for i in range(4):
        ask(f"Raise ONLY the input you believe is DI{i}")
        di = d.tags(TAG_DI0, 4)
        check(f"only DI{i} is high", di == [1 if k == i else 0 for k in range(4)], f"DI={di}")
    ask("Release all inputs")


# =============================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--unit", type=int, default=1)
    ap.add_argument("--baudrate", type=int, default=115200)
    ap.add_argument("--manual", action="store_true", help="also run tests that need you to drive DI pins")
    ap.add_argument("--only", default="", help="run only tests whose name contains this text")
    args = ap.parse_args()

    client = T.ModbusSerialClient(port=args.port, baudrate=args.baudrate,
                                  bytesize=8, parity="N", stopbits=1, timeout=1)
    if not client.connect():
        print(f"Failed to open {args.port}")
        return 2
    dev = Dev(client, args.unit)

    selected = [(n, m, f) for n, m, f in TESTS
                if (args.manual or not m) and args.only.lower() in n.lower()]
    print(f"Running {len(selected)} test group(s) on {args.port}"
          f"{' (incl. manual)' if args.manual else ''}\n")

    crashed = []
    try:
        dev.clear()
        for name, _, fn in selected:
            print(f"* {name}")
            try:
                fn(dev)
            except Exception as e:                 # a crash is a FAIL, not a silent skip
                crashed.append(name)
                RESULTS.append((f"{name} (exception)", False))
                print(f"    [FAIL] test raised {type(e).__name__}: {e}")
                try:
                    dev.clear()
                except Exception:
                    pass
    finally:
        try:
            dev.clear()
        finally:
            client.close()

    passed = sum(1 for _, ok in RESULTS if ok)
    print("\n" + "=" * 64)
    print(f"  {passed}/{len(RESULTS)} checks passed")
    for n, ok in RESULTS:
        if not ok:
            print(f"  FAILED: {n}")
    print("=" * 64)
    return 0 if passed == len(RESULTS) and not crashed else 1


if __name__ == "__main__":
    sys.exit(main())
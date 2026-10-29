#!/usr/bin/env python3
"""
test_mbw_live.py - MB_WRITE live test: checks the FRAMES the board puts on the RS-485 bus
(decision 48 in docs/handoff.md, plc_mbw). test_mbw.py only checks the configuration path.

Needs two serial ports:
    * the board's App link (USB-CDC or RS485)        this script       e.g. COM14
    * a USB-RS485 dongle on the board's RS485 bus    simulated slave   e.g. COM35

The slave simulator (mbm_slave_sim.py) runs INSIDE this script, in a thread, on the dongle
port: it answers FC05/06/15/16 like a real slave and keeps every write request it receives,
decoded, so each case compares the frames with a value computed here. Do not start
mbm_slave_sim.py on the same dongle. Source tags and trigger tags are driven through a diag
session (ENTER_DIAG / write / EXIT_DIAG) and the status tags are read back over the App link.

WARNING: WRITES Flash (rule table + MB_WRITE blocks + port table + tag types, a few times per
case) and ends with CLEAR_RULES. Dev board only. Firmware: SPLC_MBM_ENABLE=1, SPLC_MBW_ENABLE=1,
SPLC_REAL_ENABLE=1 (cases R, M need REAL), >= 30 VREG and >= 4 VFLAG tags.

Cases (run them all, or pick some with --only; names in brackets):
  [direct]   Direct FC16 INT16 / UINT16 / INT32 / UINT32, saturation, a register no pin covers is
             written 0 (the sim registers are pre-filled with 0xAAAA to prove it)
  [endian]   the four 32-bit word / byte orders ABCD CDAB BADC DCBA
  [fc06]     Direct FC06 (one 16-bit register), BADC / DCBA byte swap
  [real]     REAL pins: REAL source tag, integer source tag, multiplier; NaN source tag ->
             nothing sent, err_code 6; the next good value clears the error
  [mult]     multiplier on integer pins: rounding half away from zero, UINT16 clamp
  [fc05]     FC05 coil: 0xFF00 / 0x0000
  [fc15]     FC15 coils: bit packing LSB first, coils the pins do not cover are written 0
  [channel]  ChannelIndexed, not exclusive: parameter frame (FC16) then command (FC06);
             command 0 sends no parameter; channel 0 and channel > count -> err_code 5, no frame
  [excl]     ChannelIndexed, Exclusive: the other lit channel is cleared first, then parameter,
             then command; a channel is cleared only once
  [noparam]  ChannelIndexed with no parameter tag: the command only
  [edge]     trigger rising edge: one job per edge, none while the tag stays 1; an edge that
             arrives during a job is remembered and starts the next job
  [change]   trigger data change: Direct writes once after COMMIT (baseline), then only when a
             source tag changes, at most one job per interval; ChannelIndexed has no baseline
  [cyclic]   trigger cyclic: a frame every interval; a Deploy that disables the block stops it
  [status]   busy = 1 while a frame is on the bus, done pulses ONCE per successful job
  [errors]   slave faults: timeout, CRC, exception, cut frame, late answer -> error tag and
             err_code 1 / 2 / 3 / 1|2 / 1; retries repeat the frame (exception is not
             repeated); the next good job clears the error

What is checked about each frame: function code, start address, quantity and every value (and
for FC05 / FC15 the bytes on the wire), the order of the frames, the state of the simulated
slave, the status tags (done counted by a rule that fires on its rising edge, busy, error,
err_code) and that nothing else was sent.
The expected values come from the encoding rules of plc_mbw.c (decision 48: rounding half away
from zero, 16-bit clamp, a REAL tag is read as float32). They are the firmware's assumptions,
the ICD does not state them.

Usage (same --baud as the port table the script writes, 9600 by default):
    python test_mbw_live.py COM14 COM35
    python test_mbw_live.py COM14 COM35 --rs485-port 1 --baud 9600 --only direct,fc15
    python test_mbw_live.py COM14 COM35 --list

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open a port, 3 = link error.
"""
import argparse
import math
import struct
import sys
import time

import test_report as R   # Excel evidence report (--report / --no-report)
import test_mqtt as T
import test_tagtype as TT
import test_mbw as W
import mbm_slave_sim as SIM
from test_mqtt import check, info, LinkError

NONE = W.NONE
DT_BOOL, DT_INT16, DT_UINT16, DT_INT32, DT_UINT32, DT_REAL = (W.DT_BOOL, W.DT_INT16, W.DT_UINT16,
                                                              W.DT_INT32, W.DT_UINT32, W.DT_REAL)
EN_ABCD, EN_CDAB, EN_BADC, EN_DCBA = 0, 1, 2, 3
TRG_RISE, TRG_CHANGE, TRG_CYCLIC = 0, 1, 2          # MB_WRITE trigger modes (block +10)
RULE_ON_RISE = 1                                    # SPLC_TRG_ON_RISE of the Rule Engine
SLAVE = 1
N_BLOCKS = 4

# ------------------------------------------------------------------ settings (set by main)
PORT_NO = 1        # RS485 port of the device the blocks write on (1..4)
BAUD, PARITY, STOP = 9600, "N", 1
RETRIES = 1        # the retry count written to the port table (a failed frame is sent 1 + RETRIES times)


# ------------------------------------------------------------------ float helpers (float32 like the MCU)
def f32(x):
    try:
        return struct.unpack(">f", struct.pack(">f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def fbits(f):
    return struct.unpack(">I", struct.pack(">f", f))[0]


def bits_f(u):
    return struct.unpack(">f", struct.pack(">I", u & 0xFFFFFFFF))[0]


def swap16(v):
    return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF)


def enc16(v, en):
    v &= 0xFFFF
    return swap16(v) if en in (EN_BADC, EN_DCBA) else v


def enc32(u, en):
    u &= 0xFFFFFFFF
    hi, lo = u >> 16, u & 0xFFFF
    return {EN_ABCD: [hi, lo], EN_CDAB: [lo, hi],
            EN_BADC: [swap16(hi), swap16(lo)], EN_DCBA: [swap16(lo), swap16(hi)]}[en]


def f2i(x):
    """Round to nearest, halves away from zero, saturating (plc_mbw.c f2i)."""
    if x >= 2147483520.0:
        return 2147483647
    if x <= -2147483648.0:
        return -2147483648
    return int(f32(x + 0.5)) if x >= 0 else int(f32(x - 0.5))


def f2u(x):
    if x <= 0.0:
        return 0
    if x >= 4294967040.0:
        return 0xFFFFFFFF
    return int(f32(x + 0.5))


# ------------------------------------------------------------------ a pin: wire image + expected registers
class Pin:
    """tag: index; val: what the source tag holds (a Python int for an INT32 / BOOL tag, a float
    for a REAL tag, real=True); off / dt / en / mult as in the block."""

    def __init__(self, tag, off, dt, val, en=EN_ABCD, mult=1.0, real=False):
        self.tag, self.off, self.dt, self.val, self.en, self.mult, self.real = tag, off, dt, val, en, mult, real

    def wire(self):
        return W.pin(self.tag, self.off, self.dt, self.en, self.mult)

    def tag_u32(self):
        return fbits(self.val) if self.real else self.val & 0xFFFFFFFF

    def encode(self):
        """(offset, [registers]) the firmware must write, or None when it must send nothing."""
        m = f32(self.mult)
        unity = (m == 0.0 or m == 1.0)
        if m == 0.0:
            m = 1.0
        srcf = f32(self.val) if self.real else None
        fv = None
        if self.dt == DT_REAL or self.real or not unity:
            base = srcf if self.real else f32(float(self.val))
            fv = f32(base * m)
            if math.isnan(fv) or math.isinf(fv):
                return None
        raw_int = (not self.real) and unity
        if self.dt == DT_BOOL:
            on = (srcf != 0.0) if self.real else (self.val != 0)
            return self.off, [1 if on else 0]
        if self.dt in (DT_INT16, DT_UINT16):
            iv = self.val if raw_int else f2i(fv)
            iv = max(-32768, min(32767, iv)) if self.dt == DT_INT16 else max(0, min(65535, iv))
            return self.off, [enc16(iv, self.en)]
        if self.dt == DT_INT32:
            iv = self.val if raw_int else f2i(fv)
            return self.off, enc32(iv, self.en)
        if self.dt == DT_UINT32:
            uv = (self.val & 0xFFFFFFFF) if raw_int else f2u(fv)
            return self.off, enc32(uv, self.en)
        return self.off, enc32(fbits(fv), self.en)     # DT_REAL


def expected_regs(count, pins):
    """Registers of a Direct frame, or None when a pin cannot be encoded (NaN / Inf)."""
    regs = [0] * count
    for p in pins:
        e = p.encode()
        if e is None:
            return None
        off, vals = e
        regs[off:off + len(vals)] = vals
    return regs


# ------------------------------------------------------------------ the tag plan
class Layout:
    """Tags the cases use, built from DeviceResourceInfo:
         trig(b)    VFLAG vf0+b          trigger tag of block b
         src(i)     VREG  vr0+i          i = 0..3 pin source tags
         ch/cmd/par VREG  vr0+4..6       channel block tags
         st(b,k)    VREG  vr0+8+4b+k     done, busy, error, err_code of block b
         cnt(b)     VREG  vr0+24+b       counts the rising edges of done(b): the done pulse lasts
                                         one scan, too short to catch by polling
    """

    def __init__(self, di, do, ai, vflag, vreg, retain):
        self.vf0 = di + do + ai
        self.vr0 = self.vf0 + vflag
        self.vflag, self.vreg = vflag, vreg

    def trig(self, b):
        return self.vf0 + b

    def src(self, i):
        return self.vr0 + i

    @property
    def ch(self):
        return self.vr0 + 4

    @property
    def cmd(self):
        return self.vr0 + 5

    @property
    def par(self):
        return self.vr0 + 6

    def st(self, b, k):
        return self.vr0 + 8 + 4 * b + k

    def cnt(self, b):
        return self.vr0 + 24 + b


def counter_rules(L):
    """One rule per block: ON_RISE of the done tag -> INC_COUNTER cnt(b)."""
    return [T.encode_rule(RULE_ON_RISE, T.OP_NONE, T.ACT_INC, L.st(b, 0), L.cnt(b), action_param=1)
            for b in range(N_BLOCKS)]


# ------------------------------------------------------------------ block builders
def direct_block(L, b, fc, start, count, pins, trig_mode=TRG_RISE, interval=500, timeout=300):
    return W.block(fc=fc, slave=SLAVE, start=start, count=count, interval=interval, timeout=timeout,
                   trig_mode=trig_mode, trig_tag=L.trig(b), done=L.st(b, 0), busy=L.st(b, 1),
                   err=L.st(b, 2), errcode=L.st(b, 3), port=PORT_NO - 1,
                   pins=[p.wire() for p in pins])


def channel_block(L, b, count, base_cmd, base_par, exclusive=False, with_param=True,
                  trig_mode=TRG_RISE, interval=500, timeout=300):
    return W.block(mode=1, exclusive=exclusive, slave=SLAVE, start=base_cmd, count=count,
                   param_base=base_par, interval=interval, timeout=timeout, trig_mode=trig_mode,
                   trig_tag=L.trig(b), done=L.st(b, 0), busy=L.st(b, 1), err=L.st(b, 2),
                   errcode=L.st(b, 3), port=PORT_NO - 1,
                   channel=(L.ch, L.cmd, L.par if with_param else NONE))


# ------------------------------------------------------------------ the board (App link)
class Board:
    """Everything the cases need from the device. The PC self-check replaces this class."""

    def __init__(self, d):
        self.d = d

    def preflight(self):
        """Returns the Layout, or None when the board cannot run the test."""
        d = self.d
        r = d.read(T.REG_RESOURCE, 10)
        di, do, ai, vflag, vreg, retain = r[3], r[4], r[5], r[6], r[7], r[8]
        print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} RETAIN={retain}")
        if vreg < 30 or vflag < 4:
            print("Need >= 30 VREG and >= 4 VFLAG tags")
            return None
        resp = d.write_regs(W.REG_MBW, [0])
        if resp is not None:
            check(False, "0x1100 accepts a write: firmware built with SPLC_MBW_ENABLE=1 (needs SPLC_MBM_ENABLE=1)")
            return None
        n, err = d.try_read(0x0A0A, 1)
        if err is None:
            info(f"RS485_PORT_COUNT (0x0A0A) = {n[0]}")
            if not check(PORT_NO <= n[0], f"the device has RS485 port {PORT_NO} (it has {n[0]})"):
                return None
        caps, cerr = d.try_read(0x0A08, 2)
        if cerr is None:
            v = (caps[0] << 16) | caps[1]
            if not check(v & 0x100 != 0, f"0x0A08 announces MODBUS_MASTER (caps 0x{v:08X})"):
                return None
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)        # also drops the probe draft
        check(s == T.ST_DONE and e == 0, f"clean start: CLEAR_RULES result {s}/{e}")
        return Layout(di, do, ai, vflag, vreg, retain)

    def deploy(self, L, blocks, real_tags=(), retries=None, what="Deploy"):
        """Like the App: tag types, port table, the four blocks (those not given disabled), rules, COMMIT."""
        d = self.d
        retries = RETRIES if retries is None else retries
        resp = TT.write_types(d, {t: TT.REAL for t in real_tags})
        if resp is not None:
            return check(False, f"{what}: tag type table write failed ({resp})")
        if not write_port(d, retries):
            return False
        full = {i: blocks.get(i, W.block(enabled=False)) for i in range(N_BLOCKS)}
        if W.write_blocks(d, full) is not None:
            return check(False, f"{what}: block write failed")
        s, e = T.commit(d, counter_rules(L))
        ok = check(s == T.CFG_READY and e == 0, f"{what}: COMMIT ok (status {s}, error {e})")
        time.sleep(0.3)
        return ok

    def set_tags(self, values):
        """ENTER_DIAG, write {tag: uint32}, EXIT_DIAG. The engine (and MB_WRITE) stands still in
        between; the values stay when it runs again."""
        d = self.d
        d.fc06(T.REG_DIAG, T.CMD_ENTER)
        if not TT.wait_diag_state(d, TT.ST_DIAG):
            return check(False, "ENTER_DIAG reached DIAG_CONTROL")
        ok = True
        for tag, v in values.items():
            resp = d.fc16(T.REG_TAGS + 2 * tag, [(v >> 16) & 0xFFFF, v & 0xFFFF])
            if resp.isError():
                ok = check(False, f"diag write of tag {tag} failed ({resp})")
        d.fc06(T.REG_DIAG, T.CMD_EXIT)
        return check(TT.wait_diag_state(d, TT.ST_ENGINE), "EXIT_DIAG back to ENGINE_RUNNING") and ok

    def tag(self, idx):
        return TT.tag_value(self.d, idx)

    def clear(self):
        s, e = T.run_syscmd(self.d, T.SYS_CLEAR_RULES)
        return check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")


def port_table_image(table, retries):
    """The 64-register port table with RS485 port PORT_NO enabled at the simulator's line settings
    (same record as test_mpm.write_port_table(), but with the retry count of this test). The other
    records of `table` are kept."""
    import test_mpm as M
    new, b = list(table), (PORT_NO - 1) * M.PORT_REGS
    new[b + 0] = 0x0100 | PORT_NO
    new[b + 1], new[b + 2] = BAUD >> 16, BAUD & 0xFFFF
    new[b + 3] = M.PARITY[PARITY]
    new[b + 4] = (8 << 8) | STOP
    new[b + 5], new[b + 6], new[b + 7] = 200, 20, retries
    return new


def write_port(d, retries):
    """Draft: RS485 port PORT_NO enabled with the line settings of the simulator."""
    import test_mpm as M
    resp = d.write_regs(M.REG_PORT_TABLE, port_table_image(d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS), retries))
    return check(resp is None, f"port table: RS485 port {PORT_NO} {BAUD} baud {PARITY}{STOP}, retry {retries} (draft)")


# ------------------------------------------------------------------ the simulated slave
class Sim:
    def __init__(self, ser, unit):
        self.slave = SIM.Slave(unit)
        self.server = SIM.SimServer(ser, self.slave, quiet=True)

    def start(self):
        self.server.start()

    def stop(self):
        self.server.stop()

    def prefill(self, regs=None, coils=None):
        for a, v in (regs or {}).items():
            self.slave.regs[a] = v
        for a, v in (coils or {}).items():
            self.slave.coils[a] = v

    def collect(self, mark, n, timeout=4.0, quiet=0.4):
        """Wait for at least n new write frames (n = 0: just watch for `quiet`), then until the bus
        has been quiet for `quiet` s, so that an extra frame shows up. Returns the new frames."""
        t_end = time.time() + timeout
        while len(self.slave.writes_since(mark)) < n and time.time() < t_end:
            time.sleep(0.01)
        last_n, t_quiet = -1, time.time()
        while True:
            cur = len(self.slave.writes_since(mark))
            if cur != last_n:
                last_n, t_quiet = cur, time.time()
            if time.time() - t_quiet >= quiet:
                break
            time.sleep(0.01)
        return self.slave.writes_since(mark)


def fmt_frames(frames):
    out = []
    for fc, addr, vals in frames:
        if fc in (5, 15):
            out.append(f"FC{fc:02d} @{addr} bits {''.join(str(v) for v in vals)}")
        else:
            out.append(f"FC{fc:02d} @{addr} [" + " ".join(f"{v:04X}" for v in vals) + "]")
    return "; ".join(out) if out else "(none)"


def as_tuples(frames):
    return [(f["fc"], f["addr"], list(f["vals"]) if f["vals"] is not None else None) for f in frames]


def check_frames(label, got, want):
    g = as_tuples(got)
    ok = g == want
    if ok:
        check(True, f"{label}: {fmt_frames(g)}")
    else:
        check(False, f"{label}: got {fmt_frames(g)}  expected {fmt_frames(want)}")
    return ok


# ------------------------------------------------------------------ job helpers
def wait_idle(B, L, blk, timeout=8.0):
    """Wait until the busy tag of the block is 0."""
    t_end = time.time() + timeout
    while time.time() < t_end:
        if B.tag(L.st(blk, 1)) == 0:
            return True
        time.sleep(0.03)
    return check(False, f"block {blk}: busy tag still 1 after {timeout:.0f} s")


def status(B, L, blk):
    """(done count, error, err_code, busy) of the block."""
    return (B.tag(L.cnt(blk)), B.tag(L.st(blk, 2)), B.tag(L.st(blk, 3)), B.tag(L.st(blk, 1)))


def u32_of(v):
    return v & 0xFFFFFFFF


def fire(B, S, L, blk, label, sets, want, err=0, codes=(0,), quiet=0.4, timeout=4.0, frames_only=False):
    """One edge-triggered job: set the tags + raise the trigger tag in ONE diag session (the engine
    sees the rise when it runs again), wait for the frames, compare, check the status tags, then
    lower the trigger tag. want = list of (fc, addr, values)."""
    mark = S.slave.mark()
    cnt0 = B.tag(L.cnt(blk))
    vals = {t: u32_of(v) for t, v in sets.items()}
    vals[L.trig(blk)] = 1
    B.set_tags(vals)
    frames = S.collect(mark, len(want), timeout, quiet)
    wait_idle(B, L, blk)
    check_frames(label, frames, want)
    if not frames_only:
        cnt1, e, code, busy = status(B, L, blk)
        want_done = 1 if err == 0 else 0
        check(cnt1 - cnt0 == want_done, f"{label}: done pulsed {cnt1 - cnt0} time(s), expect {want_done}")
        check(e == err and code in codes, f"{label}: error={e} err_code={code}, expect {err} / {codes}")
    B.set_tags({L.trig(blk): 0})
    time.sleep(0.1)
    return frames


def pin_sets(pins):
    return {p.tag: p.tag_u32() for p in pins}


def retag(pins, vals):
    """Same pins with new source values."""
    for p, v in zip(pins, vals):
        p.val = v
    return pins


def all_trigs_low(B, L):
    B.set_tags({L.trig(b): 0 for b in range(N_BLOCKS)})


# ------------------------------------------------------------------ the cases
def case_direct(B, S, L):
    print("\n[direct] FC16: INT16 / UINT16 / INT32 / UINT32, saturation, an uncovered register is 0")
    pins = [Pin(L.src(0), 0, DT_INT16, -2), Pin(L.src(1), 1, DT_UINT16, 40000),
            Pin(L.src(2), 2, DT_INT32, -100000, EN_CDAB), Pin(L.src(3), 4, DT_UINT32, -1294967296, EN_DCBA)]
    # registers 26, 27 (offset 6, 7) have no pin: pre-filled, they must be written 0
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 8, pins)}, what="Deploy FC16 block"):
        return
    for name, vals in (("typical values", (-2, 40000, -100000, -1294967296)),
                       ("saturation INT16 / UINT16, INT32 min, UINT32 max", (100000, -5, -2147483648, -1)),
                       ("zeros", (0, 0, 0, 0))):
        retag(pins, vals)
        S.prefill(regs={26: 0xAAAA, 27: 0xAAAA})
        mark_regs = list(range(20, 28))
        want = expected_regs(8, pins)
        fire(B, S, L, 0, name, pin_sets(pins), [(16, 20, want)])
        got = [S.slave.regs[a] for a in mark_regs]
        check(got == want, f"{name}: slave registers 20..27 = {fmt_frames([(16, 20, got)])}, expect all written (uncovered = 0)")


def case_endian(B, S, L):
    print("\n[endian] 32-bit word / byte order ABCD CDAB BADC DCBA (0x12345678)")
    pins = [Pin(L.src(0), 0, DT_INT32, 0x12345678, EN_ABCD), Pin(L.src(0), 2, DT_INT32, 0x12345678, EN_CDAB),
            Pin(L.src(0), 4, DT_INT32, 0x12345678, EN_BADC), Pin(L.src(0), 6, DT_UINT32, 0x12345678, EN_DCBA)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 8, pins)}, what="Deploy endian block"):
        return
    want = [0x1234, 0x5678, 0x5678, 0x1234, 0x3412, 0x7856, 0x7856, 0x3412]
    check(expected_regs(8, pins) == want, "(self-check) the expected registers are the four orders of 0x12345678")
    fire(B, S, L, 0, "ABCD CDAB BADC DCBA", {L.src(0): 0x12345678}, [(16, 20, want)])


def case_fc06(B, S, L):
    print("\n[fc06] FC06, one 16-bit register, byte swap")
    for en, name in ((EN_BADC, "BADC"), (EN_DCBA, "DCBA"), (EN_ABCD, "ABCD")):
        pins = [Pin(L.src(0), 0, DT_INT16, 0x1234, en)]
        if not B.deploy(L, {0: direct_block(L, 0, 6, 21, 1, pins)}, what=f"Deploy FC06 {name}"):
            return
        fire(B, S, L, 0, f"FC06 {name} 0x1234", pin_sets(pins), [(6, 21, expected_regs(1, pins))])
        retag(pins, [-2])
        fire(B, S, L, 0, f"FC06 {name} -2", pin_sets(pins), [(6, 21, expected_regs(1, pins))])
        check(S.slave.regs[21] == expected_regs(1, pins)[0], f"{name}: slave register 21 = {S.slave.regs[21]:04X}")


def case_real(B, S, L):
    print("\n[real] REAL pins: REAL tag, integer tag, multiplier; NaN -> err_code 6")
    pins = [Pin(L.src(0), 0, DT_REAL, 23.5, EN_ABCD, real=True), Pin(L.src(1), 2, DT_REAL, 7, EN_BADC),
            Pin(L.src(2), 4, DT_REAL, -1.25, EN_CDAB, 2.0, real=True)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 6, pins)}, real_tags=(L.src(0), L.src(2)),
                    what="Deploy REAL block (source tags 0 and 2 are REAL)"):
        return
    want = expected_regs(6, pins)
    info(f"expected 23.5 ABCD, 7.0 BADC, -2.5 CDAB: {fmt_frames([(16, 20, want)])}")
    fire(B, S, L, 0, "REAL values", pin_sets(pins), [(16, 20, want)])
    nan_sets = dict(pin_sets(pins))
    nan_sets[L.src(0)] = 0x7FC00000
    fire(B, S, L, 0, "NaN source: nothing is sent, err_code 6", nan_sets, [], err=1, codes=(6,))
    inf_sets = dict(pin_sets(pins))
    inf_sets[L.src(2)] = 0x7F800000
    fire(B, S, L, 0, "Inf source (x multiplier): nothing is sent, err_code 6", inf_sets, [], err=1, codes=(6,))
    retag(pins, [100.25, -3, 0.5])
    fire(B, S, L, 0, "a good value after the failure clears the error", pin_sets(pins), [(16, 20, expected_regs(6, pins))])


def case_mult(B, S, L):
    print("\n[mult] multiplier on integer pins: rounding half away from zero, UINT16 clamp")
    pins = [Pin(L.src(0), 0, DT_INT16, 1001, EN_ABCD, 0.5), Pin(L.src(1), 1, DT_INT16, -1001, EN_ABCD, 0.5),
            Pin(L.src(2), 2, DT_INT32, -12345678, EN_ABCD, 0.001), Pin(L.src(3), 4, DT_UINT16, 30000, EN_ABCD, 3.0)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 5, pins)}, what="Deploy multiplier block"):
        return
    want = expected_regs(5, pins)
    info(f"expected 1001*0.5 -> 501, -1001*0.5 -> -501, -12345678*0.001 -> -12346, 30000*3 -> 65535: "
         f"{fmt_frames([(16, 20, want)])}")
    fire(B, S, L, 0, "multiplier and rounding", pin_sets(pins), [(16, 20, want)])
    pins = [Pin(L.src(0), 0, DT_INT16, 10, EN_ABCD, 0.0), Pin(L.src(1), 1, DT_INT16, 3, EN_ABCD, 0.25),
            Pin(L.src(2), 2, DT_INT32, 7, EN_ABCD, 1.0), Pin(L.src(3), 4, DT_UINT16, 1, EN_ABCD, 0.5)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 5, pins)}, what="Deploy multiplier 0 / 0.25 / 1 / 0.5 block"):
        return
    want = expected_regs(5, pins)
    info(f"expected multiplier 0 = no scaling (10), 3*0.25 = 0.75 -> 1, 7, 1*0.5 = 0.5 -> 1: {fmt_frames([(16, 20, want)])}")
    fire(B, S, L, 0, "multiplier 0 means 1, small values round", pin_sets(pins), [(16, 20, want)])


def case_fc05(B, S, L):
    print("\n[fc05] FC05 coil, 0xFF00 / 0x0000")
    pins = [Pin(L.src(0), 0, DT_BOOL, 7)]
    if not B.deploy(L, {0: direct_block(L, 0, 5, 3, 1, pins)}, what="Deploy FC05 block"):
        return
    S.prefill(coils={3: False})
    fr = fire(B, S, L, 0, "FC05 on (tag = 7)", pin_sets(pins), [(5, 3, [1])])
    if fr:
        check(fr[0]["raw"][4:6] == bytes([0xFF, 0x00]), f"FC05 on: value bytes {fr[0]['raw'][4:6].hex().upper()} = FF00")
    check(S.slave.coils[3] is True, "the slave coil 3 is set")
    retag(pins, [0])
    fr = fire(B, S, L, 0, "FC05 off (tag = 0)", pin_sets(pins), [(5, 3, [0])])
    if fr:
        check(fr[0]["raw"][4:6] == bytes([0x00, 0x00]), f"FC05 off: value bytes {fr[0]['raw'][4:6].hex().upper()} = 0000")
    check(S.slave.coils[3] is False, "the slave coil 3 is clear")


def case_fc15(B, S, L):
    print("\n[fc15] FC15 coils: LSB first, uncovered coils 0")
    pins = [Pin(L.src(0), 0, DT_BOOL, 1), Pin(L.src(1), 3, DT_BOOL, 5), Pin(L.src(2), 9, DT_BOOL, -1),
            Pin(L.src(3), 5, DT_BOOL, 0)]
    if not B.deploy(L, {0: direct_block(L, 0, 15, 4, 10, pins)}, what="Deploy FC15 block"):
        return
    for name, vals, bits in (("pins 0, 3, 9 set", (1, 5, -1, 0), [1, 0, 0, 1, 0, 0, 0, 0, 0, 1]),
                             ("pins 5, 3, 9 set, 0 clear", (0, 3, 1, 1), [0, 0, 0, 1, 0, 1, 0, 0, 0, 1])):
        retag(pins, vals)
        S.prefill(coils={4 + i: True for i in range(10)})       # all set: the frame must clear what it does not cover
        fr = fire(B, S, L, 0, name, pin_sets(pins), [(15, 4, bits)])
        if fr:
            packed = fr[0].get("packed", b"")
            want_packed = bytes([sum(b << i for i, b in enumerate(bits[0:8])), sum(b << i for i, b in enumerate(bits[8:10]))])
            check(packed == want_packed, f"{name}: data bytes {packed.hex().upper()} = {want_packed.hex().upper()}")
        got = [int(S.slave.coils[4 + i]) for i in range(10)]
        check(got == bits, f"{name}: slave coils 4..13 = {''.join(map(str, got))}")


def case_channel(B, S, L):
    print("\n[channel] ChannelIndexed, not exclusive: parameter (FC16), then command (FC06)")
    # 8 channels: command registers 32..39, parameter registers 48 + 2 * (ch - 1)
    if not B.deploy(L, {1: channel_block(L, 1, 8, 32, 48)}, what="Deploy ChannelIndexed block"):
        return
    ch, cmd, par = L.ch, L.cmd, L.par
    fire(B, S, L, 1, "channel 3 on, parameter 0x00012345", {ch: 3, cmd: 1, par: 0x00012345},
         [(16, 52, [0x0001, 0x2345]), (6, 34, [1])])
    check(S.slave.regs[34] == 1 and S.slave.regs[52:54] == [1, 0x2345], "slave: command register 34 = 1, parameter 52..53 = 0001 2345")
    fire(B, S, L, 1, "channel 3 off: the command only", {ch: 3, cmd: 0, par: 0x00012345}, [(6, 34, [0])])
    fire(B, S, L, 1, "channel 8 (the last): addresses 39 and 62", {ch: 8, cmd: 7, par: 0xFFFF0001},
         [(16, 62, [0xFFFF, 0x0001]), (6, 39, [7])])
    fire(B, S, L, 1, "channel 0: err_code 5, nothing sent", {ch: 0, cmd: 1, par: 1}, [], err=1, codes=(5,))
    fire(B, S, L, 1, "channel 9 (count is 8): err_code 5, nothing sent", {ch: 9, cmd: 1, par: 1}, [], err=1, codes=(5,))
    fire(B, S, L, 1, "a good channel clears the error", {ch: 1, cmd: 2, par: 9},
         [(16, 48, [0, 9]), (6, 32, [2])])


def case_excl(B, S, L):
    print("\n[excl] ChannelIndexed, Exclusive: clear the other lit channel, parameter, command")
    if not B.deploy(L, {1: channel_block(L, 1, 8, 32, 48, exclusive=True)}, what="Deploy Exclusive block"):
        return
    ch, cmd, par = L.ch, L.cmd, L.par
    fire(B, S, L, 1, "channel 1 on (nothing else lit)", {ch: 1, cmd: 1, par: 100},
         [(16, 48, [0, 100]), (6, 32, [1])])
    fire(B, S, L, 1, "channel 2 on: channel 1 is cleared first", {ch: 2, cmd: 1, par: 200},
         [(6, 32, [0]), (16, 50, [0, 200]), (6, 33, [1])])
    fire(B, S, L, 1, "channel 4 on: only channel 2 is cleared (1 already was)", {ch: 4, cmd: 1, par: 300},
         [(6, 33, [0]), (16, 54, [0, 300]), (6, 35, [1])])
    fire(B, S, L, 1, "channel 4 off: nothing else is touched", {ch: 4, cmd: 0, par: 300}, [(6, 35, [0])])
    fire(B, S, L, 1, "channel 5 on after all are off: no clear frame", {ch: 5, cmd: 1, par: 400},
         [(16, 56, [0, 400]), (6, 36, [1])])
    lit = [a for a in range(32, 40) if S.slave.regs[a] != 0]
    check(lit == [36], f"slave: only command register 36 is non-zero (non-zero: {lit})")


def case_noparam(B, S, L):
    print("\n[noparam] ChannelIndexed without a parameter tag: the command only")
    if not B.deploy(L, {1: channel_block(L, 1, 8, 32, 48, with_param=False)}, what="Deploy block with no parameter tag"):
        return
    fire(B, S, L, 1, "channel 2 on", {L.ch: 2, L.cmd: 1}, [(6, 33, [1])])


def case_edge(B, S, L):
    print("\n[edge] trigger rising edge: one job per edge; an edge during a job is remembered")
    pins = [Pin(L.src(0), 0, DT_INT16, 11)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 1, pins, timeout=2500)}, what="Deploy edge-triggered block"):
        return
    t = L.trig(0)
    mark = S.slave.mark()
    B.set_tags({L.src(0): 11, t: 1})
    fr = S.collect(mark, 1)
    check_frames("rising edge: one frame", fr, [(16, 20, [11])])
    time.sleep(0.8)
    check(len(S.slave.writes_since(mark)) == 1, "the tag stays 1 for 0.8 s: no second frame")
    B.set_tags({t: 0})
    fr = S.collect(mark, 1, quiet=0.5)
    check(len(fr) == 1, "the falling edge sends nothing")
    B.set_tags({L.src(0): 12, t: 1})
    fr = S.collect(mark, 2)
    check_frames("second rising edge", fr[1:], [(16, 20, [12])])
    B.set_tags({t: 0})
    time.sleep(0.2)

    print("   an edge during a job (the slave answers after 1.2 s) is remembered")
    S.server.delay_ms = 1200
    try:
        mark = S.slave.mark()
        B.set_tags({L.src(0): 21, t: 1})        # job 1 starts
        B.set_tags({t: 0})                      # falls while job 1 waits for its answer
        B.set_tags({L.src(0): 22, t: 1})        # rises again during job 1: remembered
        fr = S.collect(mark, 2, timeout=6.0, quiet=1.6)
        check(len(fr) == 2, f"two jobs for two edges (frames: {len(fr)})")
        if len(fr) == 2:
            check(fr[1]["t"] - fr[0]["t"] >= 1.0, f"job 2 started after job 1 ended ({fr[1]['t'] - fr[0]['t']:.2f} s apart)")
            check(as_tuples(fr)[0][2] == [21] and as_tuples(fr)[1][2] == [22],
                  f"values 21 then 22 (job 2 reads the tag when its frame is built: {as_tuples(fr)[0][2]}, {as_tuples(fr)[1][2]})")
        wait_idle(B, L, 0)
    finally:
        S.server.delay_ms = 0
    B.set_tags({t: 0})


def case_change(B, S, L):
    print("\n[change] trigger data change: baseline after COMMIT (Direct), then only on a change")
    interval = 600
    pins = [Pin(L.src(0), 0, DT_INT16, 5)]
    B.deploy(L, {}, what="Deploy with every block disabled (all tags INT32 again)")
    B.set_tags({L.src(0): 5})
    mark = S.slave.mark()
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 1, pins, trig_mode=TRG_CHANGE, interval=interval)},
                    what="Deploy Direct data-change block"):
        return
    fr = S.collect(mark, 1, timeout=3.0, quiet=1.2)
    check_frames("baseline: one frame after COMMIT, none after it while nothing changes", fr, [(16, 20, [5])])
    t_first = fr[0]["t"] if fr else time.monotonic()
    mark = S.slave.mark()
    B.set_tags({L.src(0): 6})
    fr = S.collect(mark, 1, quiet=0.3)
    check_frames("the tag changed: one frame", fr, [(16, 20, [6])])
    t2 = fr[0]["t"] if fr else time.monotonic()
    mark = S.slave.mark()
    B.set_tags({L.src(0): 7})                   # right away: inside the interval
    fr = S.collect(mark, 1, quiet=0.3)
    if check(len(fr) == 1, f"the next change: one frame (frames: {len(fr)})"):
        gap = fr[0]["t"] - t2
        check(gap >= interval / 1000.0 - 0.06, f"at most one job per interval: {gap:.2f} s after the previous one (>= {interval} ms)")
        check(as_tuples(fr)[0][2] == [7], f"it carries the newest value {as_tuples(fr)[0][2]}")
    mark = S.slave.mark()
    time.sleep(1.2)
    check(len(S.slave.writes_since(mark)) == 0, "no change, no frame (1.2 s)")

    print("   ChannelIndexed: no baseline after COMMIT")
    B.set_tags({L.ch: 2, L.cmd: 1, L.par: 10})
    mark = S.slave.mark()
    if not B.deploy(L, {1: channel_block(L, 1, 8, 32, 48, trig_mode=TRG_CHANGE, interval=300)},
                    what="Deploy ChannelIndexed data-change block"):
        return
    fr = S.collect(mark, 0, quiet=1.2)
    check(len(fr) == 0, f"nothing is sent after COMMIT (frames: {len(fr)})")
    mark = S.slave.mark()
    B.set_tags({L.cmd: 0})
    fr = S.collect(mark, 1, quiet=0.4)
    check_frames("the command tag changed to 0: channel 2 off", fr, [(6, 33, [0])])
    mark = S.slave.mark()
    B.set_tags({L.ch: 3, L.cmd: 1, L.par: 77})
    fr = S.collect(mark, 2, quiet=0.4)
    check_frames("channel, command and parameter changed: one job", fr, [(16, 52, [0, 77]), (6, 34, [1])])


def case_cyclic(B, S, L):
    print("\n[cyclic] trigger cyclic: a frame every interval; disabling the block stops it")
    interval = 300
    pins = [Pin(L.src(0), 0, DT_INT16, 3)]
    B.deploy(L, {}, what="Deploy with every block disabled (all tags INT32 again)")
    B.set_tags({L.src(0): 3})
    mark = S.slave.mark()
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 1, pins, trig_mode=TRG_CYCLIC, interval=interval)},
                    what="Deploy cyclic block (300 ms)"):
        return
    time.sleep(2.0)
    fr = S.slave.writes_since(mark)
    n = len(fr)
    check(5 <= n <= 8, f"frames in 2.0 s at 300 ms: {n} (expect about 6..7)")
    gaps = [fr[i + 1]["t"] - fr[i]["t"] for i in range(len(fr) - 1)]
    if gaps:
        check(min(gaps) >= 0.25, f"no gap shorter than the interval (shortest {min(gaps):.2f} s)")
        check(max(gaps) <= 0.6, f"no gap much longer than the interval (longest {max(gaps):.2f} s)")
    check(all(as_tuples([f])[0] == (16, 20, [3]) for f in fr), "every frame carries FC16 @20 [0003]")
    B.deploy(L, {}, what="Deploy with every block disabled")
    time.sleep(0.3)
    mark = S.slave.mark()
    time.sleep(1.2)
    check(len(S.slave.writes_since(mark)) == 0, "after the Deploy that disables the block: no frame for 1.2 s")


def case_status(B, S, L):
    print("\n[status] busy while a frame is on the bus, done pulses once")
    pins = [Pin(L.src(0), 0, DT_INT16, 4)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 1, pins, timeout=2500)}, what="Deploy status block"):
        return
    S.server.delay_ms = 500
    try:
        cnt0 = B.tag(L.cnt(0))
        mark = S.slave.mark()
        B.set_tags({L.trig(0): 1})
        seen_busy, seen_idle_after = False, False
        t_end = time.time() + 4.0
        while time.time() < t_end:
            busy = B.tag(L.st(0, 1))
            if busy == 1:
                seen_busy = True
            elif seen_busy:
                seen_idle_after = True
                break
            time.sleep(0.02)
        check(seen_busy, "busy was 1 while the slave held its answer (500 ms)")
        check(seen_idle_after, "busy went back to 0 after the answer")
        S.collect(mark, 1)
        time.sleep(0.2)
        cnt1, e, code, busy = status(B, L, 0)
        check(cnt1 - cnt0 == 1, f"done pulsed exactly once ({cnt1 - cnt0})")
        check(e == 0 and code == 0, f"error={e} err_code={code}")
        check(B.tag(L.st(0, 0)) == 0, "the done tag is back to 0")
    finally:
        S.server.delay_ms = 0
    B.set_tags({L.trig(0): 0})


def case_errors(B, S, L):
    print("\n[errors] slave faults and retries (port retry count %d)" % RETRIES)
    pins = [Pin(L.src(0), 0, DT_INT16, 9)]
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 1, pins, timeout=300)}, what="Deploy error-test block"):
        return
    sets = pin_sets(pins)
    n_retry = 1 + RETRIES
    one = [(16, 20, [9])]
    for fault, frames, codes, text in (
            ("timeout", n_retry, (1,), "no answer"),
            ("crc", n_retry, (2,), "wrong CRC"),
            ("short", n_retry, (1, 2), "cut frame"),
            ("exception", 1, (3,), "slave exception 04 (not repeated)")):
        S.slave.fault = fault
        try:
            fire(B, S, L, 0, f"{text}: {frames} frame(s), error 1, err_code {codes}", sets, one * frames,
                 err=1, codes=codes, timeout=6.0, quiet=0.8)
        finally:
            S.slave.fault = "none"
    fire(B, S, L, 0, "the next good job clears error and err_code", sets, one)

    print("   late answer: the slave answers 500 ms after the 300 ms timeout (no retry)")
    if not B.deploy(L, {0: direct_block(L, 0, 16, 20, 1, pins, timeout=300)}, retries=0, what="Deploy the same block with retry 0"):
        return
    S.server.delay_ms = 500
    try:
        fire(B, S, L, 0, "late answer: err_code 1, one frame", sets, one, err=1, codes=(1,), timeout=4.0, quiet=0.8)
    finally:
        S.server.delay_ms = 0
    time.sleep(0.5)
    fire(B, S, L, 0, "the late bytes did not spoil the next job", sets, one)


CASES = [("direct", case_direct), ("endian", case_endian), ("fc06", case_fc06), ("real", case_real),
         ("mult", case_mult), ("fc05", case_fc05), ("fc15", case_fc15), ("channel", case_channel),
         ("excl", case_excl), ("noparam", case_noparam), ("edge", case_edge), ("change", case_change),
         ("cyclic", case_cyclic), ("status", case_status), ("errors", case_errors)]


def run_cases(B, S, only=None):
    L = B.preflight()
    if L is None:
        return
    info(f"tags: trigger tags {L.trig(0)}..{L.trig(3)}, sources {L.src(0)}..{L.src(3)}, "
         f"channel/command/parameter {L.ch}/{L.cmd}/{L.par}, status {L.st(0, 0)}..{L.st(3, 3)}, counters {L.cnt(0)}..{L.cnt(3)}")
    B.set_tags({L.trig(b): 0 for b in range(N_BLOCKS)})
    S.slave.reset()
    try:
        for name, fn in CASES:
            if only and name not in only:
                continue
            S.slave.reset()
            fn(B, S, L)
    finally:
        print("\nCleanup (CLEAR_RULES)")
        B.clear()
        time.sleep(0.5)
        mark = S.slave.mark()
        time.sleep(1.0)
        check(len(S.slave.writes_since(mark)) == 0, "after CLEAR_RULES nothing is written on the bus")


# ------------------------------------------------------------------ main
def main():
    global PORT_NO, BAUD, PARITY, STOP, RETRIES, SLAVE
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="board App-link port, e.g. COM14")
    p.add_argument("sim_port", nargs="?", help="USB-RS485 dongle for the simulated slave, e.g. COM35")
    p.add_argument("--unit", type=int, default=1, help="Modbus unit of the board's App link")
    p.add_argument("--baudrate", type=int, default=115200, help="App-link baud rate (RS485 App link only)")
    p.add_argument("--chunk", type=int, default=48)
    p.add_argument("--rs485-port", type=int, choices=[1, 2, 3, 4], default=1,
                   help="RS485 port of the device the blocks write on (default 1: the only port of the Zigbee-IO)")
    p.add_argument("--baud", type=int, default=9600, help="line speed of that port and of the simulator (default 9600)")
    p.add_argument("--parity", choices=["N", "E", "O"], default="N")
    p.add_argument("--stop", type=int, choices=[1, 2], default=1)
    p.add_argument("--retries", type=int, choices=range(0, 6), default=1,
                   help="retry count written to the port table (default 1)")
    p.add_argument("--slave", type=int, default=1, help="Modbus address the simulator answers and the blocks write to")
    p.add_argument("--only", help="comma separated case names (see --list)")
    p.add_argument("--list", action="store_true", help="list the cases and exit")
    R.add_args(p)
    args = p.parse_args()
    if args.list:
        for name, fn in CASES:
            print(f"  {name:8s} {(fn.__doc__ or '').strip()}")
        return
    if not args.port or not args.sim_port:
        p.error("the board port and the simulator (dongle) port are required")
    only = None
    if args.only:
        only = [x.strip() for x in args.only.split(",") if x.strip()]
        bad = [x for x in only if x not in dict(CASES)]
        if bad:
            p.error(f"unknown case(s): {', '.join(bad)}")
    R.start(args)
    PORT_NO, BAUD, PARITY, STOP, RETRIES = args.rs485_port, args.baud, args.parity, args.stop, args.retries
    SLAVE = args.slave
    T.Dev.chunk = max(1, min(args.chunk, 120))

    try:
        import serial
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial")
        sys.exit(2)
    par = {"N": serial.PARITY_NONE, "O": serial.PARITY_ODD, "E": serial.PARITY_EVEN}[PARITY]
    stop = serial.STOPBITS_ONE if STOP == 1 else serial.STOPBITS_TWO
    try:
        ser = serial.Serial(args.sim_port, BAUD, bytesize=8, parity=par, stopbits=stop, timeout=0.005)
    except Exception as e:
        print(f"Cannot open the simulator port {args.sim_port}: {e}")
        sys.exit(2)
    client = T.make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        ser.close()
        sys.exit(2)

    d = T.Dev(client, args.unit)
    S = Sim(ser, SLAVE)
    S.start()
    print(f"Simulated slave {SLAVE} on {args.sim_port} at {BAUD} 8{PARITY}{STOP}; blocks write on RS485 port {PORT_NO}, retry {RETRIES}")
    link_error = None
    try:
        run_cases(Board(d), S, only)
    except LinkError as e:
        link_error = e
        print(f"\n  [LINK] {e}")
        T.cleanup_after_abort(d)
    except Exception:
        import traceback
        traceback.print_exc()
        print("\n  [ERROR] unexpected exception in the test script (not a firmware verdict)")
        T.cleanup_after_abort(d)
        T._failures += 1
    finally:
        S.stop()
        for c in (d.c, ser):
            try:
                c.close()
            except Exception:
                pass
    if T.Dev.retried or T.Dev.retried_w:
        print(f"\n(info) lost frames needing a retry: {T.Dev.retried} read(s), {T.Dev.retried_w} write(s)")
    if link_error is not None:
        print("\nINCONCLUSIVE: a request outcome was unknown (link error), NOT a firmware failure."
              f" {T._failures} check(s) failed before it.")
        sys.exit(3)
    print("\nALL PASS" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
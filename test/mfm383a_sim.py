#!/usr/bin/env python3
"""
mfm383a_sim.py - Modbus RTU slave simulator of the Selec MFM383A-C multifunction meter
(3-phase 4-wire, RS485 version). Source: Selec OP623-V01 "MODBUS REGISTER ADDRESSES LIST".

Run it on the PC on a USB-RS485 dongle wired A-A, B-B to the RS485 terminal of the
gateway that MB_READ uses (port 2 of the 0x1400 port table), then Deploy from Studio:

    python mfm383a_sim.py COM20                       # unit 1, 9600 8N1 (meter factory setting)
    python mfm383a_sim.py COM20 --unit 3 --baud 19200 --parity E --stop 1
    python mfm383a_sim.py COM20 --speed 60 --trace    # energy counters run 60x faster
    python mfm383a_sim.py --selftest                  # no hardware: checks the register map

Register map (same as the manual; addresses below are the HEX protocol addresses):

  FC04 - Input registers (the "3xxxx" table), read only, 2 registers = 1 float32 each:
    0x00 V1N    0x02 V2N    0x04 V3N    0x06 Avg V LN
    0x08 V12    0x0A V23    0x0C V31    0x0E Avg V LL
    0x10 I1     0x12 I2     0x14 I3     0x16 Avg I
    0x18 kW1    0x1A kW2    0x1C kW3
    0x1E kVA1   0x20 kVA2   0x22 kVA3
    0x24 kVAr1  0x26 kVAr2  0x28 kVAr3
    0x2A Total kW   0x2C Total kVA   0x2E Total kVAr
    0x30 PF1    0x32 PF2    0x34 PF3    0x36 Avg PF    0x38 Frequency
    0x3A kWh    0x3C kVAh   0x3E kVArh  (total energy, float32, counts up while running)
    0x40 Serial number (2 registers, HEX, not a float)
    Readable from any address 0x00..0x41 (a start on an odd address returns half floats).

  FC03 / FC06 / FC16 - Holding registers (the "4xxxx" table), 1 register each, integer:
    0x00 Password 0..9998        0x01 N/W selection 0=3P-4W 1=3P-3W
    0x02 CT secondary 1 or 5     0x03 CT primary (5..10000 if CT sec=5, else 1..10000)
    0x04 PT secondary 100..500   0x05..0x06 PT primary 100..10000 (2 registers, High Word first)
    0x07 Slave id 1..255 (applied right after the answer is sent)
    0x08 Baud code 0..6 (300,600,1200,2400,4800,9600,19200)   0x09 Parity 0 None 1 Odd 2 Even
    0x0A Stop bit 0=1 stop, 1=2 stops
    0x0B Factory default (write 1)   0x0C/0x0D/0x0E reset kWh / kVAh / kVArh (write 1)
    0x0F Auto mode pages 1..20       0x10..0x23 page sequence 1..20
    0x24 Backlight 0..7200 s         0x25 Energy selection 0=Active 1=All
  Baud / parity / stop written over Modbus are STORED and read back but the line settings of
  this simulator are NOT changed (restart it with --baud/--parity/--stop).

What the manual does not say, so these are assumptions you can change:
  * Float word order. Default ABCD (big endian, High Word first) = Studio Endianness 0.
    If you see garbage, try --float-order cdab (Studio Endianness 1) / badc (2) / dcba (3).
    On a REAL meter confirm with one value you know (frequency ~ 50.0).
  * FC03 reads ONLY the holding table and FC04 ONLY the input table (as the manual lists them).
  * The answer is sent --delay-ms after the request (default 20 ms; the meter's maximum is 100 ms).

Suggested MB_READ blocks in Studio (FC04, slave = --unit, 9600 8N1, Real pins, multiplier 1;
a block has at most 4 pins, so 4 blocks = 16 values; the destination tags must be REAL tags):
    Block 1  start 0x00  count 8 : V1N (off 0), V2N (2), V3N (4), Avg V LN (6)
    Block 2  start 0x10  count 8 : I1 (0), I2 (2), I3 (4), Avg I (6)
    Block 3  start 0x2A  count 14: Total kW (0), Total kVA (2), Total kVAr (4), Avg PF (12)
    Block 4  start 0x38  count 8 : Frequency (0), kWh (2), kVAh (4), kVArh (6)

Fault injection (applies to EVERY answer), to see the block error codes:
    --fault timeout (err 1)   --fault crc (err 2)   --fault exception (err 3)   --fault short (err 4 or 1)
"""

import argparse
import math
import random
import struct
import sys
import time

# ------------------------------------------------------------------ register maps
INPUT_REGS = 0x42            # 0x00..0x41
HOLD_REGS = 0x26             # 0x00..0x25

# (hex address, name) of every float in the input table
INPUT_NAMES = {
    0x00: "V1N", 0x02: "V2N", 0x04: "V3N", 0x06: "AvgVLN",
    0x08: "V12", 0x0A: "V23", 0x0C: "V31", 0x0E: "AvgVLL",
    0x10: "I1", 0x12: "I2", 0x14: "I3", 0x16: "AvgI",
    0x18: "kW1", 0x1A: "kW2", 0x1C: "kW3",
    0x1E: "kVA1", 0x20: "kVA2", 0x22: "kVA3",
    0x24: "kVAr1", 0x26: "kVAr2", 0x28: "kVAr3",
    0x2A: "TotkW", 0x2C: "TotkVA", 0x2E: "TotkVAr",
    0x30: "PF1", 0x32: "PF2", 0x34: "PF3", 0x36: "AvgPF", 0x38: "Freq",
    0x3A: "kWh", 0x3C: "kVAh", 0x3E: "kVArh", 0x40: "Serial",
}
HOLD_NAMES = {
    0x00: "Password", 0x01: "N/W", 0x02: "CTsec", 0x03: "CTpri", 0x04: "PTsec", 0x05: "PTpri",
    0x07: "SlaveId", 0x08: "Baud", 0x09: "Parity", 0x0A: "Stop", 0x0B: "FactoryDefault",
    0x0C: "ResetkWh", 0x0D: "ResetkVAh", 0x0E: "ResetkVArh", 0x0F: "AutoPages",
    0x24: "Backlight", 0x25: "EnergySel",
}
for _i in range(0x10, 0x24):
    HOLD_NAMES[_i] = f"Page{_i - 0x0F}"

BAUDS = [300, 600, 1200, 2400, 4800, 9600, 19200]
EXC_ILLEGAL_FUNCTION, EXC_ILLEGAL_ADDRESS, EXC_ILLEGAL_VALUE, EXC_DEVICE_FAILURE = 1, 2, 3, 4


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(body: bytes) -> bytes:
    return body + struct.pack("<H", crc16(body))


def pack_float(value: float, order: str):
    """float32 -> two registers. order: abcd (High Word first, big endian), cdab, badc, dcba."""
    b = struct.pack(">f", value)                   # A B C D
    a, bb, c, d = b[0], b[1], b[2], b[3]
    seq = {"abcd": (a, bb, c, d), "cdab": (c, d, a, bb), "badc": (bb, a, d, c), "dcba": (d, c, bb, a)}[order]
    return (seq[0] << 8) | seq[1], (seq[2] << 8) | seq[3]


def unpack_float(r0: int, r1: int, order: str) -> float:
    s = bytes([r0 >> 8, r0 & 0xFF, r1 >> 8, r1 & 0xFF])
    abcd = {"abcd": s, "cdab": s[2:] + s[:2], "badc": bytes([s[1], s[0], s[3], s[2]]),
            "dcba": s[::-1]}[order]
    return struct.unpack(">f", abcd)[0]


# ------------------------------------------------------------------ the meter model
class Meter:
    """A 3-phase 4-wire load that drifts slowly, plus energy counters that integrate it."""

    def __init__(self, order="abcd", load=1.0, speed=1.0, serial=0x00A1B2C3,
                 kwh0=12345.6, seed=None):
        self.order = order
        self.load = load                           # 1.0 = about 10 A per phase
        self.speed = speed                         # energy integrates `speed` x faster than real time
        self.serial = serial & 0xFFFFFFFF
        self.rng = random.Random(seed)
        self.t0 = time.monotonic()
        self.last = self.t0
        self.kwh, self.kvah, self.kvarh = kwh0, kwh0 * 1.06, kwh0 * 0.33     # float64 inside, float32 on the wire
        self.base_v = [230.0, 231.5, 229.0]
        self.base_i = [10.0, 12.0, 9.0]
        self.base_pf = [0.95, 0.92, 0.97]
        self.values = {}
        self.input = [0] * INPUT_REGS
        self.hold = [0] * HOLD_REGS
        self.factory_defaults()
        self.update(self.t0)

    def factory_defaults(self):
        h = self.hold
        h[:] = [0] * HOLD_REGS
        h[0x00] = 10                               # password
        h[0x01] = 0                                # 3P-4W
        h[0x02] = 5                                # CT secondary
        h[0x03] = 5                                # CT primary
        h[0x04] = 350                              # PT secondary
        h[0x05], h[0x06] = 0, 350                  # PT primary (2 registers, High Word first)
        h[0x07] = 1                                # slave id
        h[0x08] = 5                                # 9600
        h[0x09] = 0                                # none
        h[0x0A] = 0                                # 1 stop bit
        h[0x0F] = 20
        for i in range(20):
            h[0x10 + i] = i + 1
        h[0x24] = 0
        h[0x25] = 0

    def reset_energy(self, which: str):
        if which in ("kwh", "all"):
            self.kwh = 0.0
        if which in ("kvah", "all"):
            self.kvah = 0.0
        if which in ("kvarh", "all"):
            self.kvarh = 0.0

    def update(self, now: float):
        dt = now - self.last
        self.last = now
        t = now - self.t0
        v, i, pf = [], [], []
        for p in range(3):
            ph = 2.0 * math.pi * p / 3.0
            v.append(self.base_v[p] + 1.2 * math.sin(t / 7.0 + ph) + self.rng.uniform(-0.15, 0.15))
            i.append(max(0.0, self.base_i[p] * self.load * (1.0 + 0.25 * math.sin(t / 11.0 + ph)
                                                            + self.rng.uniform(-0.01, 0.01))))
            pf.append(min(1.0, max(0.5, self.base_pf[p] + 0.02 * math.sin(t / 13.0 + ph))))
        kva = [v[p] * i[p] / 1000.0 for p in range(3)]
        kw = [kva[p] * pf[p] for p in range(3)]
        kvar = [math.sqrt(max(0.0, kva[p] ** 2 - kw[p] ** 2)) for p in range(3)]
        tot_kw, tot_kva, tot_kvar = sum(kw), sum(kva), sum(kvar)
        avg_pf = tot_kw / tot_kva if tot_kva > 0 else 1.0
        vll = [math.sqrt(3.0) * (v[a] + v[b]) / 2.0 for a, b in ((0, 1), (1, 2), (2, 0))]
        freq = 50.0 + 0.04 * math.sin(t / 5.0) + self.rng.uniform(-0.005, 0.005)

        hours = dt * self.speed / 3600.0
        self.kwh += tot_kw * hours
        self.kvah += tot_kva * hours
        self.kvarh += tot_kvar * hours

        vals = [*v, sum(v) / 3.0, *vll, sum(vll) / 3.0, *i, sum(i) / 3.0, *kw, *kva, *kvar,
                tot_kw, tot_kva, tot_kvar, *pf, avg_pf, freq, self.kwh, self.kvah, self.kvarh]
        names = [0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16,
                 0x18, 0x1A, 0x1C, 0x1E, 0x20, 0x22, 0x24, 0x26, 0x28, 0x2A, 0x2C, 0x2E,
                 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A, 0x3C, 0x3E]
        assert len(vals) == len(names)
        for addr, val in zip(names, vals):
            self.values[addr] = val
            self.input[addr], self.input[addr + 1] = pack_float(val, self.order)
        self.input[0x40], self.input[0x41] = self.serial >> 16, self.serial & 0xFFFF

    def summary(self) -> str:
        g = self.values
        return (f"V={g[0x00]:.1f}/{g[0x02]:.1f}/{g[0x04]:.1f}V  I={g[0x10]:.2f}/{g[0x12]:.2f}/{g[0x14]:.2f}A  "
                f"P={g[0x2A]:.3f}kW  PF={g[0x36]:.3f}  f={g[0x38]:.2f}Hz  "
                f"E={g[0x3A]:.3f}kWh")


# ------------------------------------------------------------------ the Modbus slave
class Slave:
    def __init__(self, unit: int, meter: Meter, fault: str = "none"):
        self.unit = unit
        self.meter = meter
        self.fault = fault
        self.requests = 0
        self.pending_unit = None                   # a new slave id is applied after the answer is sent
        self.meter.hold[0x07] = unit

    def _exc(self, fc: int, code: int) -> bytes:
        return with_crc(bytes([self.unit, fc | 0x80, code]))

    def handle(self, frame: bytes):
        """Bytes to send, or None for no answer."""
        if len(frame) < 4:
            return None
        if crc16(frame[:-2]) != struct.unpack("<H", frame[-2:])[0]:
            print("  ! bad CRC in the request, ignored")
            return None
        unit, fc = frame[0], frame[1]
        if unit != self.unit:
            return None                            # not for us: a real slave stays silent
        self.requests += 1
        if self.fault == "timeout":
            return None
        if self.fault == "exception":
            return self._exc(fc, EXC_DEVICE_FAILURE)
        rsp = self._answer(fc, frame[2:-2])
        if self.fault == "crc":
            rsp = rsp[:-1] + bytes([rsp[-1] ^ 0xFF])
        elif self.fault == "short":
            rsp = rsp[:max(3, len(rsp) - 4)]
        return rsp

    def describe(self, fc: int, pdu: bytes) -> str:
        """One readable line for the log (never raises)."""
        try:
            if fc in (3, 4) and len(pdu) == 4:
                a, q = struct.unpack(">HH", pdu)
                names = INPUT_NAMES if fc == 4 else HOLD_NAMES
                hit = [names[x] for x in range(a, a + q) if x in names]
                return f"FC{fc:02d} read  0x{a:04X} x{q}  [{', '.join(hit)}]"
            if fc == 6 and len(pdu) == 4:
                a, v = struct.unpack(">HH", pdu)
                return f"FC06 write 0x{a:04X} = {v} [{HOLD_NAMES.get(a, '?')}]"
            if fc == 16 and len(pdu) >= 5:
                a, q = struct.unpack(">HH", pdu[:4])
                return f"FC16 write 0x{a:04X} x{q}"
        except struct.error:
            pass
        return f"FC{fc:02d}"

    def _answer(self, fc: int, pdu: bytes) -> bytes:
        u, m = self.unit, self.meter
        if fc in (3, 4):
            if len(pdu) != 4:
                return self._exc(fc, EXC_ILLEGAL_VALUE)
            addr, qty = struct.unpack(">HH", pdu)
            if not 1 <= qty <= 125:
                return self._exc(fc, EXC_ILLEGAL_VALUE)
            table, size = (m.input, INPUT_REGS) if fc == 4 else (m.hold, HOLD_REGS)
            if addr + qty > size:
                return self._exc(fc, EXC_ILLEGAL_ADDRESS)
            data = b"".join(struct.pack(">H", table[addr + k]) for k in range(qty))
            return with_crc(bytes([u, fc, 2 * qty]) + data)

        if fc == 6:
            if len(pdu) != 4:
                return self._exc(fc, EXC_ILLEGAL_VALUE)
            addr, value = struct.unpack(">HH", pdu)
            err = self._write(addr, [value])
            return self._exc(fc, err) if err else with_crc(bytes([u, fc]) + pdu)

        if fc == 16:
            if len(pdu) < 5:
                return self._exc(fc, EXC_ILLEGAL_VALUE)
            addr, qty, nbytes = struct.unpack(">HHB", pdu[:5])
            if not 1 <= qty <= 123 or nbytes != 2 * qty or len(pdu) != 5 + nbytes:
                return self._exc(fc, EXC_ILLEGAL_VALUE)
            values = [struct.unpack(">H", pdu[5 + 2 * k:7 + 2 * k])[0] for k in range(qty)]
            err = self._write(addr, values)
            return self._exc(fc, err) if err else with_crc(bytes([u, fc]) + pdu[:4])

        return self._exc(fc, EXC_ILLEGAL_FUNCTION)

    # --- holding table writes: all-or-nothing, range checked like the manual's table
    def _valid(self, h, a: int, touched_pt: bool) -> bool:
        v = h[a]
        if a == 0x00:
            return 0 <= v <= 9998
        if a == 0x01:
            return v in (0, 1)
        if a == 0x02:
            return v in (1, 5)
        if a == 0x03:
            return (5 if h[0x02] == 5 else 1) <= v <= 10000
        if a == 0x04:
            return 100 <= v <= 500
        if a in (0x05, 0x06):
            return (not touched_pt) or 100 <= ((h[0x05] << 16) | h[0x06]) <= 10000
        if a == 0x07:
            return 1 <= v <= 255
        if a == 0x08:
            return 0 <= v <= 6
        if a == 0x09:
            return 0 <= v <= 2
        if a == 0x0A:
            return v in (0, 1)
        if 0x0B <= a <= 0x0E:
            return v == 1
        if a == 0x0F or 0x10 <= a <= 0x23:
            return 1 <= v <= 20
        if a == 0x24:
            return 0 <= v <= 7200
        if a == 0x25:
            return v in (0, 1)
        return False

    def _write(self, addr: int, values):
        m = self.meter
        if addr + len(values) > HOLD_REGS:
            return EXC_ILLEGAL_ADDRESS
        trial = list(m.hold)
        for k, v in enumerate(values):
            trial[addr + k] = v
        touched = range(addr, addr + len(values))
        pt = any(a in (0x05, 0x06) for a in touched)
        for a in touched:
            if not self._valid(trial, a, pt):
                return EXC_ILLEGAL_VALUE
        # commit, then run the momentary actions
        for a in touched:
            m.hold[a] = trial[a]
        if 0x0B in touched:
            m.factory_defaults()
            print("  * factory default applied")
        for a, which in ((0x0C, "kwh"), (0x0D, "kvah"), (0x0E, "kvarh")):
            if a in touched:
                m.reset_energy(which)
                m.hold[a] = 0
                print(f"  * {which} reset to 0")
        if 0x0B in touched:
            m.hold[0x0B] = 0
        if 0x07 in touched or 0x0B in touched:
            self.pending_unit = m.hold[0x07]
        if any(a in touched for a in (0x08, 0x09, 0x0A)):
            print("  * baud/parity/stop stored; the simulator line settings are NOT changed")
        return 0

    def apply_pending(self):
        if self.pending_unit is not None and self.pending_unit != self.unit:
            print(f"  * slave id is now {self.pending_unit}")
            self.unit = self.pending_unit
        self.pending_unit = None


# ------------------------------------------------------------------ framing (as mbm_slave_sim.py)
STALE_S = 0.3


def request_len(buf):
    """Length of the request at buf[0], or None while it cannot be told yet."""
    if len(buf) < 2:
        return None
    if buf[1] in (15, 16):
        return None if len(buf) < 7 else 9 + buf[6]
    return 8


def hexs(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b)


# ------------------------------------------------------------------ self test (no hardware)
def selftest() -> int:
    bad = 0

    def check(ok, what):
        nonlocal bad
        print(f"  [{'PASS' if ok else 'FAIL'}] {what}")
        bad += 0 if ok else 1

    def req(slave, fc, addr, x=None, vals=None):
        if fc in (3, 4):
            body = struct.pack(">BBHH", slave.unit, fc, addr, x)
        elif fc == 6:
            body = struct.pack(">BBHH", slave.unit, fc, addr, x)
        else:
            body = struct.pack(">BBHHB", slave.unit, fc, addr, len(vals), 2 * len(vals)) + \
                   b"".join(struct.pack(">H", v) for v in vals)
        return slave.handle(with_crc(body))

    print("selftest: register map and Modbus behaviour")
    for order in ("abcd", "cdab", "badc", "dcba"):
        m = Meter(order=order, seed=1)
        s = Slave(1, m)
        r = req(s, 4, 0x38, 2)                      # frequency
        ok = r is not None and crc16(r[:-2]) == struct.unpack("<H", r[-2:])[0] and r[1] == 4 and r[2] == 4
        regs = struct.unpack(">HH", r[3:7]) if ok else (0, 0)
        f = unpack_float(regs[0], regs[1], order) if ok else 0.0
        check(ok and 49.9 < f < 50.1, f"{order}: FC04 0x38 = frequency {f:.3f} Hz")
    m = Meter(seed=1)
    s = Slave(1, m)
    r = req(s, 4, 0x00, 8)
    v = [unpack_float(*struct.unpack(">HH", r[3 + 4 * k:7 + 4 * k]), "abcd") for k in range(4)]
    check(all(220 < x < 240 for x in v), f"V1N,V2N,V3N,AvgLN = {[round(x, 1) for x in v]}")
    r = req(s, 4, 0x40, 2)
    check(r[3:7] == bytes([0x00, 0xA1, 0xB2, 0xC3]), "serial number 0x00A1B2C3 at 0x40 (hex)")
    check(req(s, 4, 0x41, 2)[1] == 0x84 and req(s, 4, 0x41, 2)[2] == 2, "read crossing 0x41 -> exception 02")
    check(req(s, 3, 0x07, 1)[3:5] == b"\x00\x01", "FC03 0x07 slave id = 1")
    check(req(s, 6, 0x07, 9)[1] == 6, "FC06 slave id = 9 accepted")
    check(s.unit == 1, "the new id is NOT active until the answer was sent")
    s.apply_pending()
    check(s.unit == 9, "...and is active afterwards")
    r = req(s, 6, 0x08, 7)
    check(r[1] == 0x86 and r[2] == 3, "FC06 baud code 7 -> exception 03")
    r = req(s, 6, 0x02, 3)
    check(r[1] == 0x86 and r[2] == 3, "FC06 CT secondary 3 -> exception 03")
    r = req(s, 16, 0x05, vals=[0, 5000])
    check(r[1] == 16, "FC16 PT primary 5000 accepted")
    r = req(s, 16, 0x05, vals=[1, 0])
    check(r[1] == 0x90 and r[2] == 3, "FC16 PT primary 65536 -> exception 03")
    check(req(s, 3, 0x05, 2)[3:7] == b"\x00\x00\x13\x88", "PT primary reads back 5000")
    m.update(m.last + 3600.0 / m.speed)
    e1 = m.values[0x3A]
    check(e1 > 12345.6, f"kWh counts up ({e1:.2f})")
    r = req(s, 6, 0x0C, 1)
    check(r[1] == 6 and m.kwh == 0.0, "FC06 0x0C = 1 resets kWh")
    check(req(s, 4, 0x00, 126)[1] == 0x84, "FC04 quantity 126 -> exception 03")
    r = s.handle(with_crc(struct.pack(">BBHH", s.unit, 1, 0, 8)))
    check(r is not None and r[1] == 0x81 and r[2] == 1, "FC01 -> exception 01 (illegal function)")
    print("selftest:", "ALL PASS" if bad == 0 else f"{bad} FAILED")
    return 0 if bad == 0 else 1


# ------------------------------------------------------------------ main loop
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port of the USB-RS485 dongle, e.g. COM20 or /dev/ttyUSB0")
    ap.add_argument("--unit", type=int, default=1, help="slave address 1..255 (default 1)")
    ap.add_argument("--baud", type=int, default=9600, choices=BAUDS, help="default 9600 (meter factory setting)")
    ap.add_argument("--parity", choices=["N", "O", "E"], default="N", help="default N")
    ap.add_argument("--stop", type=int, choices=[1, 2], default=1)
    ap.add_argument("--float-order", choices=["abcd", "cdab", "badc", "dcba"], default="abcd",
                    help="float32 word/byte order of the answers (default abcd = Studio Endianness 0)")
    ap.add_argument("--load", type=float, default=1.0, help="load factor, 1.0 = about 10 A per phase")
    ap.add_argument("--speed", type=float, default=1.0, help="energy counters run this many times faster than real time")
    ap.add_argument("--kwh-start", type=float, default=12345.6, help="initial kWh counter")
    ap.add_argument("--serial", type=lambda x: int(x, 0), default=0x00A1B2C3, help="serial number (hex ok)")
    ap.add_argument("--fault", choices=["none", "timeout", "crc", "exception", "short"], default="none")
    ap.add_argument("--delay-ms", type=float, default=20.0, help="wait this long before every answer (meter max 100)")
    ap.add_argument("--trace", action="store_true", help="print every chunk the port returns, with a timestamp")
    ap.add_argument("--quiet", action="store_true", help="do not print every request/answer")
    ap.add_argument("--status-every", type=float, default=5.0,
                    help="print the meter values every N seconds (0 = never)")
    ap.add_argument("--read-timeout-ms", type=float, default=5.0, help="pyserial read timeout")
    ap.add_argument("--selftest", action="store_true", help="check the register map without a serial port")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.port:
        ap.error("the serial port is required (or use --selftest)")
    if not 1 <= args.unit <= 255:
        ap.error("--unit must be 1..255")
    try:
        import serial
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial")
        return 2

    parity = {"N": serial.PARITY_NONE, "O": serial.PARITY_ODD, "E": serial.PARITY_EVEN}[args.parity]
    stop = serial.STOPBITS_ONE if args.stop == 1 else serial.STOPBITS_TWO
    ser = serial.Serial(args.port, args.baud, bytesize=8, parity=parity, stopbits=stop,
                        timeout=args.read_timeout_ms / 1000.0)
    meter = Meter(order=args.float_order, load=args.load, speed=args.speed, serial=args.serial,
                  kwh0=args.kwh_start)
    # keep the baud/parity/stop registers in line with how the simulator was started
    meter.hold[0x08] = BAUDS.index(args.baud)
    meter.hold[0x09] = {"N": 0, "O": 1, "E": 2}[args.parity]
    meter.hold[0x0A] = 0 if args.stop == 1 else 1
    slave = Slave(args.unit, meter, args.fault)
    print(f"MFM383A-C simulator: slave {args.unit} on {args.port} at {args.baud} 8{args.parity}{args.stop}, "
          f"floats {args.float_order.upper()}, fault={args.fault}. Ctrl+C to stop.")

    # A USB-serial dongle hands bytes over in batches (FTDI latency ~16 ms), so one request can
    # arrive in two pieces: collect bytes until a complete CRC-valid request is there.
    buf = bytearray()
    last_rx = 0.0
    last_status = time.monotonic()
    t0 = time.monotonic()
    try:
        while True:
            data = ser.read(256)
            now = time.monotonic()
            meter.update(now)
            if args.status_every > 0 and now - last_status >= args.status_every:
                last_status = now
                print(f"[{now - t0:8.1f}s] {meter.summary()}  ({slave.requests} requests)")
            if data:
                if args.trace:
                    print(f"  [{now - t0:9.3f}s] chunk n={len(data)}: {hexs(data)}")
                buf += data
                last_rx = now
            elif buf and now - last_rx > STALE_S:
                print(f"  ! {len(buf)} stale byte(s) dropped: {hexs(bytes(buf))}")
                buf.clear()
                continue
            while buf:
                n = request_len(buf)
                if n is None or len(buf) < n:
                    break
                if crc16(bytes(buf[:n - 2])) != struct.unpack("<H", bytes(buf[n - 2:n]))[0]:
                    print(f"  ! bad CRC, dropped byte {buf[0]:02X} to resynchronise")
                    del buf[0]
                    continue
                frame = bytes(buf[:n])
                del buf[:n]
                if not args.quiet and frame[0] == slave.unit:
                    print(f"RX {slave.describe(frame[1], frame[2:-2])}")
                if args.trace:
                    print(f"RX {hexs(frame)}")
                meter.update(time.monotonic())
                rsp = slave.handle(frame)
                if rsp is None:
                    continue
                if args.delay_ms > 0:
                    time.sleep(args.delay_ms / 1000.0)
                ser.write(rsp)
                ser.flush()
                if args.trace:
                    print(f"TX {hexs(rsp)}")
                slave.apply_pending()
    except KeyboardInterrupt:
        print(f"\nstopped after {slave.requests} request(s)")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
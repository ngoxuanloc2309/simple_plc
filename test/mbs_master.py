#!/usr/bin/env python3
"""
mbs_master.py - a small Modbus RTU MASTER for trying out the SimplePLC Modbus SLAVE
(plc_mbs, decision 75 in docs/handoff.md, SPLC-SPEC-SLAVE-001 map).

Requires: pip install pyserial          (no pymodbus: the RTU frames are built here, so
                                          test_mbs.py can also send broken frames)

The device side: the RS-485 port must be configured as Slave by the App (port table 0x1400,
+14 = 1, +15 = Slave ID) and the firmware built with SPLC_MODBUS_SLAVE_ENABLE = 1.

Use as a tool:
    python mbs_master.py COM35 dump                       # read the whole map, decoded
    python mbs_master.py COM35 dump --unit 5 --baudrate 115200 --parity E
    python mbs_master.py COM35 read  holding 0x20 4       # FC03   (also: coils, discrete)
    python mbs_master.py COM35 write reg 0x20 0x1234      # FC06
    python mbs_master.py COM35 write regs 0x20 0x1234 0x5678   # FC16
    python mbs_master.py COM35 write coil 1 1             # FC05
    python mbs_master.py COM35 write coils 0x64 1 0 1 1   # FC15
    python mbs_master.py COM35 poll 0x0000 4 --period 0.5 # FC03 in a loop (Ctrl+C stops)
Numbers accept 0x.. hex. Use --tcp-less: this tool is serial only.

Use as a module (test_mbs.py does):
    from mbs_master import RtuMaster
    m = RtuMaster("COM35", 115200, unit=5)
    print(m.read_holding(0x0000, 4))

Exit codes (CLI): 0 ok, 1 Modbus exception / no answer, 2 cannot open port.
"""
import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("pyserial is missing: pip install pyserial")
    sys.exit(2)


# ------------------------------------------------------------------ errors
class ModbusError(Exception):
    """Base class."""


class NoAnswer(ModbusError):
    """Nothing (or a broken frame) came back within the timeout."""


class ModbusException(ModbusError):
    """The slave answered with an exception response."""

    NAMES = {1: "ILLEGAL_FUNCTION", 2: "ILLEGAL_DATA_ADDRESS", 3: "ILLEGAL_DATA_VALUE",
             4: "SLAVE_DEVICE_FAILURE", 6: "SLAVE_DEVICE_BUSY"}

    def __init__(self, fc, code):
        self.fc, self.code = fc, code
        super().__init__(f"FC{fc:02X} exception 0x{code:02X} ({self.NAMES.get(code, '?')})")


# ------------------------------------------------------------------ helpers
def crc16(data):
    """CRC-16/MODBUS, returned as an int (low byte goes first on the wire)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def frame(unit, pdu):
    body = bytes([unit]) + bytes(pdu)
    return body + struct.pack("<H", crc16(body))


def crc_ok(f):
    return len(f) >= 4 and struct.unpack("<H", f[-2:])[0] == crc16(f[:-2])


def u32_of(regs, i=0):
    """Two registers, HIGH word first, as one unsigned 32-bit value."""
    return (regs[i] << 16) | regs[i + 1]


def f32_of(regs, i=0):
    return struct.unpack(">f", struct.pack(">I", u32_of(regs, i)))[0]


def s32(u):
    return u - (1 << 32) if u & 0x80000000 else u


def split_u32(v):
    v &= 0xFFFFFFFF
    return [v >> 16, v & 0xFFFF]


def bits_to_list(data, n):
    return [(data[i // 8] >> (i % 8)) & 1 for i in range(n)]


def pack_bits(bits):
    out = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


# ------------------------------------------------------------------ the master
class RtuMaster:
    """Blocking Modbus RTU master over a serial port. One request, one answer."""

    def __init__(self, port, baudrate=115200, unit=1, parity="N", stopbits=1, bytesize=8,
                 timeout=0.5, trace=False):
        self.unit = unit
        self.timeout = timeout
        self.trace = trace
        self.ser = serial.Serial(port=port, baudrate=baudrate, parity=parity, stopbits=stopbits,
                                 bytesize=bytesize, timeout=0.02, write_timeout=1.0)
        # 3.5 characters of silence between two frames (at least 2 ms: OS timing is coarse).
        bits = 1 + bytesize + (0 if parity == "N" else 1) + stopbits
        self.t35 = max(0.002, 3.5 * bits / baudrate)
        self._last_tx = 0.0
        self.flush()

    def close(self):
        self.ser.close()

    def flush(self):
        self.ser.reset_input_buffer()

    # ---- raw wire ----------------------------------------------------------
    def send_raw(self, data):
        """Send bytes exactly as given (no CRC added) after the 3.5-char silence."""
        wait = self.t35 - (time.monotonic() - self._last_tx)
        if wait > 0:
            time.sleep(wait)
        if self.trace:
            print("  TX", data.hex(" "))
        self.ser.write(data)
        self.ser.flush()
        self._last_tx = time.monotonic()

    def _recv_frame(self, timeout):
        """Collect one answer. Ends when the length is known from the function code, or the
        line has been quiet for 3.5 chars. Returns bytes (maybe empty)."""
        buf = bytearray()
        t_end = time.monotonic() + timeout
        t_last = None
        while time.monotonic() < t_end:
            chunk = self.ser.read(256)
            if chunk:
                buf += chunk
                t_last = time.monotonic()
                want = self._expected(buf)
                if want and len(buf) >= want:
                    break
            elif t_last is not None and time.monotonic() - t_last > max(self.t35, 0.01):
                break
        if self.trace and buf:
            print("  RX", bytes(buf).hex(" "))
        return bytes(buf)

    @staticmethod
    def _expected(buf):
        if len(buf) < 2:
            return 0
        fc = buf[1]
        if fc & 0x80:
            return 5
        if fc in (1, 2, 3, 4):
            return 5 + buf[2] if len(buf) >= 3 else 0
        if fc in (5, 6, 15, 16):
            return 8
        return 0

    def transact_raw(self, data, timeout=None):
        """Send `data` as is and return whatever answer arrives (b'' = silence)."""
        self.flush()
        self.send_raw(data)
        return self._recv_frame(self.timeout if timeout is None else timeout)

    def request(self, pdu, unit=None, timeout=None):
        """Send a PDU (function code first); return the response PDU. Raises NoAnswer on
        silence / bad CRC / wrong unit, ModbusException on an exception response."""
        u = self.unit if unit is None else unit
        rsp = self.transact_raw(frame(u, pdu), timeout)
        if not rsp:
            raise NoAnswer("no answer")
        if not crc_ok(rsp):
            raise NoAnswer(f"bad CRC in answer: {rsp.hex(' ')}")
        if rsp[0] != u:
            raise NoAnswer(f"answer from unit {rsp[0]}, expected {u}")
        body = rsp[1:-2]
        if body[0] == (pdu[0] | 0x80):
            raise ModbusException(pdu[0], body[1])
        if body[0] != pdu[0]:
            raise NoAnswer(f"answer for FC{body[0]:02X}, asked FC{pdu[0]:02X}")
        return body

    # ---- function codes ------------------------------------------------------
    def read_coils(self, addr, qty):
        b = self.request(struct.pack(">BHH", 1, addr, qty))
        return bits_to_list(b[2:], qty)

    def read_discrete(self, addr, qty):
        b = self.request(struct.pack(">BHH", 2, addr, qty))
        return bits_to_list(b[2:], qty)

    def read_holding(self, addr, qty):
        b = self.request(struct.pack(">BHH", 3, addr, qty))
        return list(struct.unpack(f">{qty}H", b[2:2 + 2 * qty]))

    def read_input(self, addr, qty):
        b = self.request(struct.pack(">BHH", 4, addr, qty))
        return list(struct.unpack(f">{qty}H", b[2:2 + 2 * qty]))

    def write_coil(self, addr, value):
        self.request(struct.pack(">BHH", 5, addr, 0xFF00 if value else 0))

    def write_reg(self, addr, value):
        self.request(struct.pack(">BHH", 6, addr, value & 0xFFFF))

    def write_coils(self, addr, bits):
        self.request(struct.pack(">BHHB", 15, addr, len(bits), (len(bits) + 7) // 8) + pack_bits(bits))

    def write_regs(self, addr, values):
        self.request(struct.pack(">BHHB", 16, addr, len(values), 2 * len(values)) +
                     struct.pack(f">{len(values)}H", *[v & 0xFFFF for v in values]))

    # ---- map decoding ------------------------------------------------------------
    def read_map(self, di=4, do=4, ai=0, vflag=32, vreg=32, vret=32, counter=8):
        """Read every region of the Slave map and decode it. Counts = what the board has."""
        out = {}
        w = self.read_holding(0x0000, 4)
        out["DI_WORD"], out["DO_WORD"] = w[0], w[1]
        out["VFLAG_DWORD"] = u32_of(w, 2)
        out["DI"] = [(w[0] >> i) & 1 for i in range(di)]
        out["DO"] = [(w[1] >> i) & 1 for i in range(do)]
        out["VFLAG"] = [(out["VFLAG_DWORD"] >> i) & 1 for i in range(vflag)]
        if ai:
            a = self.read_holding(0x0010, 8)
            out["AI"] = [f32_of(a, 2 * i) for i in range(ai)]
        if vreg:
            r = self.read_holding(0x0020, 2 * vreg)
            out["VREG"] = [s32(u32_of(r, 2 * i)) for i in range(vreg)]
        if vret:
            r = self.read_holding(0x0060, 2 * vret)
            out["VREG_R"] = [s32(u32_of(r, 2 * i)) for i in range(vret)]
        if counter:
            r = self.read_holding(0x00A0, 2 * counter)
            out["COUNTER"] = [s32(u32_of(r, 2 * i)) for i in range(counter)]
        d = self.read_holding(0x00F0, 10)
        out["SYS_DIAG"] = {"uptime_s": u32_of(d, 0), "reset_reason": d[2], "health_flags": d[3],
                           "cpu_load_percent": d[4], "ram_usage_percent": d[5],
                           "scan_time_ms": u32_of(d, 6), "max_scan_time_ms": u32_of(d, 8)}
        return out


# ------------------------------------------------------------------ CLI
def num(s):
    return int(s, 0)


def cli_dump(m, a):
    t0 = time.monotonic()
    d = m.read_map(a.di, a.do, a.ai, a.vflag, a.vreg, a.vret, a.counter)
    dt = (time.monotonic() - t0) * 1000
    print(f"Slave {m.unit}  (map read in {dt:.0f} ms)")
    print(f"  DI_WORD   0x{d['DI_WORD']:04X}   DI    {d['DI']}")
    print(f"  DO_WORD   0x{d['DO_WORD']:04X}   DO    {d['DO']}")
    print(f"  VFLAG     0x{d['VFLAG_DWORD']:08X}   set: {[i for i, v in enumerate(d['VFLAG']) if v]}")
    if "AI" in d:
        print(f"  AI        {d['AI']}")
    for k in ("VREG", "VREG_R", "COUNTER"):
        if k in d:
            nz = {i: v for i, v in enumerate(d[k]) if v}
            print(f"  {k:<9} {len(d[k])} entries, non-zero: {nz if nz else 'none'}")
    print("  SYS_DIAG ", d["SYS_DIAG"])


def main():
    ap = argparse.ArgumentParser(description="Modbus RTU master for the SimplePLC Slave map")
    ap.add_argument("port", help="serial port, e.g. COM35 or /dev/ttyUSB0")
    ap.add_argument("--unit", type=int, default=1, help="Slave ID set by the App (+15), default 1")
    ap.add_argument("--baudrate", type=int, default=115200)
    ap.add_argument("--parity", choices="NEO", default="N")
    ap.add_argument("--stopbits", type=int, choices=(1, 2), default=1)
    ap.add_argument("--timeout", type=float, default=0.5)
    ap.add_argument("--trace", action="store_true", help="print every frame")
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump", help="read the whole map")
    for n, v in (("di", 4), ("do", 4), ("ai", 0), ("vflag", 32), ("vreg", 32), ("vret", 32), ("counter", 8)):
        d.add_argument(f"--{n}", type=int, default=v, help=f"tags of that kind on the board (default {v})")

    r = sub.add_parser("read", help="one read request")
    r.add_argument("kind", choices=("coils", "discrete", "holding", "input"))
    r.add_argument("addr", type=num)
    r.add_argument("qty", type=num)

    w = sub.add_parser("write", help="one write request")
    w.add_argument("kind", choices=("coil", "reg", "coils", "regs"))
    w.add_argument("addr", type=num)
    w.add_argument("values", type=num, nargs="+")

    p = sub.add_parser("poll", help="FC03 in a loop")
    p.add_argument("addr", type=num)
    p.add_argument("qty", type=num)
    p.add_argument("--period", type=float, default=1.0)

    a = ap.parse_args()
    try:
        m = RtuMaster(a.port, a.baudrate, a.unit, a.parity, a.stopbits, timeout=a.timeout, trace=a.trace)
    except serial.SerialException as e:
        print(f"cannot open {a.port}: {e}")
        return 2
    try:
        if a.cmd == "dump":
            cli_dump(m, a)
        elif a.cmd == "read":
            fn = {"coils": m.read_coils, "discrete": m.read_discrete,
                  "holding": m.read_holding, "input": m.read_input}[a.kind]
            v = fn(a.addr, a.qty)
            if a.kind in ("holding", "input"):
                print(" ".join(f"0x{x:04X}" for x in v))
            else:
                print(v)
        elif a.cmd == "write":
            if a.kind == "coil":
                m.write_coil(a.addr, a.values[0])
            elif a.kind == "reg":
                m.write_reg(a.addr, a.values[0])
            elif a.kind == "coils":
                m.write_coils(a.addr, a.values)
            else:
                m.write_regs(a.addr, a.values)
            print("OK")
        elif a.cmd == "poll":
            try:
                while True:
                    v = m.read_holding(a.addr, a.qty)
                    print(time.strftime("%H:%M:%S"), " ".join(f"{x:04X}" for x in v))
                    time.sleep(a.period)
            except KeyboardInterrupt:
                pass
    except ModbusError as e:
        print(f"ERROR: {e}")
        return 1
    finally:
        m.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
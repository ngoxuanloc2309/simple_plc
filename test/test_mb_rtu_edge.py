#!/usr/bin/env python3
"""
test_mb_rtu_edge.py - Modbus RTU edge cases of the App link over a REAL RS485
line, to compare SPLC_MODBUS_LIB_MB (mb_master_slave) with SPLC_MODBUS_LIB_NANO
(nanoMODBUS) on the same board and the same adapter. Handoff 86/88/89.

Requires: pip install pyserial   (no pymodbus: frames and CRC are built here)

Board setup (Zigbee-IO). The App link must be on RS485 (USART1, DE = PB13,
115200 8N1), so in splc_config/splcopts.h:
    SPLC_PORT_USB_CDC        0      (App over RS485 instead of USB-CDC)
    SPLC_MBM_ENABLE          0      (MB_READ also needs USART1: the board #errors otherwise)
    SPLC_MBW_ENABLE          0
    SPLC_MODBUS_SLAVE_ENABLE 0
    SPLC_MODBUS_LIB          SPLC_MODBUS_LIB_MB   (run 1)   /  ..._NANO  (run 2)
Flash one build, run this script, flash the other, run it again, compare the
"SUMMARY" blocks. Wire the USB-RS485 adapter A/B to the board's A/B.

What each test does (every request is FC03, 1 register, default DIAG_STATE
0x0A21 which reads 1 = ENGINE_RUNNING when idle; change with --addr):
  1. SANITY   : N plain requests; reply must be a valid frame (CRC ok, same unit,
                same function). Also reports bytes that arrive AFTER the reply
                (echo / junk) and the round-trip latency (min/median/p95/max).
  2. SPLIT    : the same request written in two pieces with a pause of GAP ms in
                the middle (mid-frame). Per gap: reply rate. A gap below the
                T3.5 the board uses must still be answered; a gap above it must
                NOT (the half-frames are separate, broken frames). Shows where
                the board's end-of-frame timer really is.
  3. GARBAGE  : one stray byte glued in front of a valid frame (a) in the same
                write, (b) followed by a pause of --junk-gap-ms. (b) is the
                normal "line noise, then silence, then a real frame" case and
                should be answered; (a) is the hard one (mb_master_slave does not
                slide to find a frame start, handoff 88).
  4. RECOVERY : a bad-CRC frame, then a good one: the good one must be answered.

IMPORTANT: a USB-RS485 adapter is NOT a precise timing source. Its FIFO/latency
timer (FTDI default 16 ms) merges or delays bytes, and the OS adds jitter, so
SPLIT gaps are indicative (measured on the PC, not on the wire). Look at the
trend between MB and NANO on the same adapter, not at absolute milliseconds.
Set the adapter's latency timer to 1 ms if you can (FTDI: Device Manager ->
Port settings -> Advanced -> Latency Timer).

Examples
  python test_mb_rtu_edge.py COM35
  python test_mb_rtu_edge.py COM35 --tag MB --count 300 --unit 1
  python test_mb_rtu_edge.py COM35 --rts toggle      # adapter needs RTS for DE
  python test_mb_rtu_edge.py COM35 --only split --gaps 0,3,6,10,15,25
  python test_mb_rtu_edge.py --list
"""
import argparse
import statistics
import sys
import time

import serial
import serial.tools.list_ports


# --- Modbus RTU helpers ------------------------------------------------------

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(body: bytes) -> bytes:
    c = crc16(body)
    return body + bytes((c & 0xFF, c >> 8))


def fc03(unit: int, addr: int, qty: int = 1) -> bytes:
    return with_crc(bytes((unit, 3, addr >> 8, addr & 0xFF, qty >> 8, qty & 0xFF)))


def expected_len(first: bytes):
    """Length of the reply once its first 3 bytes are known (None = unknown yet)."""
    if len(first) < 2:
        return None
    if first[1] & 0x80:
        return 5
    if len(first) < 3:
        return None
    return 5 + first[2]


def frame_ok(frame: bytes, unit: int) -> bool:
    return (len(frame) >= 5 and frame[0] == unit and crc16(frame[:-2]) ==
            (frame[-2] | frame[-1] << 8))


# --- Serial helpers ----------------------------------------------------------

class Bus:
    def __init__(self, ser, rts_mode):
        self.ser = ser
        self.rts_mode = rts_mode

    def set_rts(self, level):
        try:
            self.ser.rts = level
        except (OSError, ValueError, serial.SerialException):
            pass   # a driver without modem-control lines (virtual port): RTS is not needed

    def drain(self, quiet_s=0.05):
        """Throw away whatever is in the line until it is quiet for quiet_s."""
        end = time.perf_counter() + quiet_s
        while time.perf_counter() < end:
            n = self.ser.in_waiting
            if n:
                self.ser.read(n)
                end = time.perf_counter() + quiet_s
            else:
                time.sleep(0.001)

    def write(self, data: bytes):
        if self.rts_mode == "toggle":
            self.set_rts(True)
        self.ser.write(data)
        self.ser.flush()
        if self.rts_mode == "toggle":
            # let the last byte leave the adapter before listening again
            time.sleep(len(data) * 10.0 / self.ser.baudrate + 0.002)
            self.set_rts(False)

    def read_reply(self, unit, timeout_s):
        """Read one reply frame. Returns (frame or None, t_done, raw_bytes)."""
        buf = bytearray()
        end = time.perf_counter() + timeout_s
        t_done = None
        while time.perf_counter() < end:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
                need = expected_len(bytes(buf))
                if need is not None and len(buf) >= need:
                    t_done = time.perf_counter()
                    break
            else:
                time.sleep(0.0005)
        return (bytes(buf[:expected_len(bytes(buf)) or 0]) if t_done else None), t_done, bytes(buf)

    def trailing(self, quiet_s=0.04):
        """Bytes that arrive after a reply (echo / junk)."""
        out = bytearray()
        end = time.perf_counter() + quiet_s
        while time.perf_counter() < end:
            n = self.ser.in_waiting
            if n:
                out += self.ser.read(n)
                end = time.perf_counter() + quiet_s
            else:
                time.sleep(0.001)
        return bytes(out)


def pct(ok, n):
    return f"{ok}/{n} ({(100.0 * ok / n) if n else 0:.0f}%)"


# --- Tests -------------------------------------------------------------------

def t_sanity(bus, a, results):
    print(f"\n[1] SANITY: {a.count} plain FC03 requests (unit {a.unit}, addr 0x{a.addr:04X})")
    req = fc03(a.unit, a.addr)
    lat, ok, extra, timeouts, bad = [], 0, 0, 0, 0
    for _ in range(a.count):
        bus.drain(0.005)
        t0 = time.perf_counter()
        bus.write(req)
        frame, t1, raw = bus.read_reply(a.unit, a.timeout / 1000.0)
        if frame is None:
            timeouts += 1
            continue
        if not frame_ok(frame, a.unit) or frame[1] != 3:
            bad += 1
            continue
        ok += 1
        lat.append((t1 - t0) * 1000.0)
        tail = bus.trailing(0.01)
        if tail:
            extra += 1
        time.sleep(a.pause / 1000.0)
    print(f"    valid replies : {pct(ok, a.count)}   timeouts: {timeouts}   bad frames: {bad}")
    print(f"    extra bytes after the reply in {extra} request(s)"
          + ("  <-- echo or junk on the line" if extra else ""))
    if lat:
        lat.sort()
        p95 = lat[min(len(lat) - 1, int(len(lat) * 0.95))]
        print(f"    round trip ms : min {lat[0]:.1f}  median {statistics.median(lat):.1f}  "
              f"p95 {p95:.1f}  max {lat[-1]:.1f}")
        results["latency_median_ms"] = statistics.median(lat)
        results["latency_p95_ms"] = p95
    results["sanity_ok"] = ok
    results["sanity_n"] = a.count
    results["extra_bytes_requests"] = extra
    return ok > 0


def t_split(bus, a, results):
    gaps = [float(x) for x in a.gaps.split(",")]
    print(f"\n[2] SPLIT: request written as {a.split_at}+{8 - a.split_at} bytes, "
          f"{a.reps} tries per gap")
    req = fc03(a.unit, a.addr)
    print("    gap ms | answered")
    out = {}
    for g in gaps:
        ok = 0
        for _ in range(a.reps):
            bus.drain(0.03)
            bus.write(req[:a.split_at])
            if g > 0:
                time.sleep(g / 1000.0)
            bus.write(req[a.split_at:])
            frame, _, _ = bus.read_reply(a.unit, a.timeout / 1000.0)
            if frame is not None and frame_ok(frame, a.unit):
                ok += 1
            time.sleep(a.pause / 1000.0)
        out[g] = ok
        print(f"    {g:6.1f} | {pct(ok, a.reps)}")
    results["split"] = out


def t_garbage(bus, a, results):
    print(f"\n[3] GARBAGE: one stray byte in front of a valid frame, {a.reps} tries each")
    req = fc03(a.unit, a.addr)
    junk_list = (0x00, 0xFF, 0xAA)
    res_a = res_b = n = 0
    for junk in junk_list:
        for _ in range(a.reps):
            n += 1
            # (a) glued: junk + frame in one write
            bus.drain(0.03)
            bus.write(bytes((junk,)) + req)
            f, _, _ = bus.read_reply(a.unit, a.timeout / 1000.0)
            if f is not None and frame_ok(f, a.unit):
                res_a += 1
            time.sleep(a.pause / 1000.0)
            # (b) junk, silence, then the frame
            bus.drain(0.03)
            bus.write(bytes((junk,)))
            time.sleep(a.junk_gap_ms / 1000.0)
            bus.write(req)
            f, _, _ = bus.read_reply(a.unit, a.timeout / 1000.0)
            if f is not None and frame_ok(f, a.unit):
                res_b += 1
            time.sleep(a.pause / 1000.0)
    print(f"    (a) junk glued to the frame            : {pct(res_a, n)}")
    print(f"    (b) junk, {a.junk_gap_ms:g} ms pause, then frame    : {pct(res_b, n)}")
    results["garbage_glued"] = (res_a, n)
    results["garbage_paused"] = (res_b, n)


def t_recovery(bus, a, results):
    print(f"\n[4] RECOVERY: bad-CRC frame, then a good one, {a.reps} tries")
    good = fc03(a.unit, a.addr)
    bad = good[:-1] + bytes(((good[-1] ^ 0xFF) & 0xFF,))
    ok = silent = 0
    for _ in range(a.reps):
        bus.drain(0.03)
        bus.write(bad)
        f, _, _ = bus.read_reply(a.unit, 0.12)
        if f is None:
            silent += 1          # expected: a bad-CRC frame must not be answered
        time.sleep(0.03)
        bus.write(good)
        f, _, _ = bus.read_reply(a.unit, a.timeout / 1000.0)
        if f is not None and frame_ok(f, a.unit):
            ok += 1
        time.sleep(a.pause / 1000.0)
    print(f"    bad frame ignored (no reply)  : {pct(silent, a.reps)}")
    print(f"    good frame answered afterwards: {pct(ok, a.reps)}")
    results["recovery_ok"] = (ok, a.reps)
    results["bad_crc_silent"] = (silent, a.reps)


# --- Main --------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="serial port of the USB-RS485 adapter, e.g. COM35")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    p.add_argument("--tag", default="", help="label for the summary, e.g. MB or NANO")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--addr", type=lambda s: int(s, 0), default=0x0A21,
                   help="holding register to read (default 0x0A21 = DIAG_STATE)")
    p.add_argument("--count", type=int, default=200, help="requests in the SANITY test")
    p.add_argument("--reps", type=int, default=20, help="tries per case in the other tests")
    p.add_argument("--gaps", default="0,2,5,8,12,20,30",
                   help="comma list of SPLIT pauses in ms")
    p.add_argument("--split-at", type=int, default=4, help="bytes in the first SPLIT piece (1..7)")
    p.add_argument("--junk-gap-ms", type=float, default=30.0)
    p.add_argument("--timeout", type=float, default=300.0, help="reply timeout, ms")
    p.add_argument("--pause", type=float, default=15.0, help="idle time between requests, ms")
    p.add_argument("--rts", choices=["off", "toggle"], default="off",
                   help="toggle = RTS high while writing (adapter that needs RTS for DE)")
    p.add_argument("--only", choices=["sanity", "split", "garbage", "recovery"],
                   help="run a single test")
    a = p.parse_args()

    if a.list or not a.port:
        for pt in serial.tools.list_ports.comports():
            print(f"{pt.device:12s} {pt.description}")
        return 0
    if not 1 <= a.split_at <= 7:
        p.error("--split-at must be 1..7")

    ser = serial.Serial(a.port, a.baud, bytesize=8, parity="N", stopbits=1, timeout=0,
                        write_timeout=1.0)
    bus = Bus(ser, a.rts)
    bus.set_rts(False)
    results = {}
    print(f"test_mb_rtu_edge: {a.port} {a.baud} 8N1 unit {a.unit} "
          f"tag '{a.tag or '-'}'  (board firmware is NOT detected: label it with --tag)")
    bus.drain(0.1)

    alive = t_sanity(bus, a, results) if a.only in (None, "sanity") else True
    if not alive:
        print("\nNo valid reply at all: check wiring A/B, baud, unit, the RTS option "
              "and that the firmware was built with SPLC_PORT_USB_CDC 0.")
        return 2
    if a.only in (None, "split"):
        t_split(bus, a, results)
    if a.only in (None, "garbage"):
        t_garbage(bus, a, results)
    if a.only in (None, "recovery"):
        t_recovery(bus, a, results)

    print(f"\n=== SUMMARY [{a.tag or '-'}] baud {a.baud} ===")
    if "sanity_ok" in results:
        print(f"  sanity            {pct(results['sanity_ok'], results['sanity_n'])}"
              + (f"  median {results['latency_median_ms']:.1f} ms  p95 {results['latency_p95_ms']:.1f} ms"
                 if "latency_median_ms" in results else ""))
        print(f"  requests with echo/junk after reply: {results['extra_bytes_requests']}")
    if "split" in results:
        print("  split  (gap ms: answered/" + str(a.reps) + "): "
              + "  ".join(f"{g:g}:{n}" for g, n in results["split"].items()))
    if "garbage_glued" in results:
        print(f"  garbage glued     {pct(*results['garbage_glued'])}")
        print(f"  garbage paused    {pct(*results['garbage_paused'])}")
    if "recovery_ok" in results:
        print(f"  recovery          {pct(*results['recovery_ok'])}   "
              f"bad-CRC silent {pct(*results['bad_crc_silent'])}")
    print("Run again with the other firmware and compare. Not a pass/fail test: "
          "the numbers show how the two libraries behave on your line.")
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
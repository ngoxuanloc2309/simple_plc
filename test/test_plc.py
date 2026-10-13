"""
test_plc.py - continuous Modbus RTU probe for the SimplePLC RS485 App link.

Sends one FC03 request (read 10 registers from 0x0000 = Device Descriptor)
over and over so the bus can be watched on an oscilloscope, and prints
whether a valid reply came back.

Usage:
    python test_plc.py COM8
    python test_plc.py COM8 --period 100 --baud 115200 --unit 1
    python test_plc.py COM8 --count 20
"""
import argparse
import sys
import time

import serial


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_request(unit: int, addr: int, qty: int) -> bytes:
    body = bytes([unit, 0x03, addr >> 8, addr & 0xFF, qty >> 8, qty & 0xFF])
    crc = crc16_modbus(body)
    return body + bytes([crc & 0xFF, crc >> 8])          # CRC little-endian


def check_reply(rx: bytes, unit: int, qty: int) -> str:
    """Return 'OK' or a short reason why the reply is not a valid FC03 answer."""
    if not rx:
        return "no reply"
    expected = 5 + 2 * qty                               # unit, fc, bytes, data, crc
    if len(rx) < 5:
        return f"too short ({len(rx)} B)"
    if crc16_modbus(rx[:-2]) != (rx[-2] | rx[-1] << 8):
        return f"bad CRC ({len(rx)} B)"
    if rx[0] != unit:
        return f"wrong unit {rx[0]}"
    if rx[1] & 0x80:
        return f"exception 0x{rx[2]:02X}"
    if len(rx) != expected:
        return f"wrong length {len(rx)} B (expected {expected})"
    return "OK"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", default="COM8", help="serial port (default COM8)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--unit", type=int, default=1)
    ap.add_argument("--period", type=float, default=200.0,
                    help="ms between requests (default 200)")
    ap.add_argument("--wait", type=float, default=100.0,
                    help="ms to wait for the reply after each request (default 100)")
    ap.add_argument("--count", type=int, default=0,
                    help="number of requests, 0 = run until Ctrl+C")
    args = ap.parse_args()

    try:
        s = serial.Serial(args.port, args.baud, 8, "N", 1, timeout=0.02)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}")
        return 1

    req = build_request(args.unit, 0x0000, 10)
    print(f"{args.port} {args.baud} 8N1, unit {args.unit}, "
          f"period {args.period:.0f} ms, Ctrl+C to stop")
    print("TX:", req.hex(" "))

    sent = ok = 0
    try:
        while args.count == 0 or sent < args.count:
            t0 = time.perf_counter()
            s.reset_input_buffer()
            s.write(req)
            s.flush()
            sent += 1

            # collect the reply until the line has been quiet for a while
            rx = bytearray()
            deadline = time.perf_counter() + args.wait / 1000.0
            while time.perf_counter() < deadline:
                chunk = s.read(64)
                if chunk:
                    rx += chunk
                    deadline = time.perf_counter() + 0.02
            verdict = check_reply(bytes(rx), args.unit, 10)
            ok += verdict == "OK"
            print(f"#{sent:<5} RX {len(rx):3d} B  {verdict:<24} {bytes(rx[:12]).hex(' ')}")

            rest = args.period / 1000.0 - (time.perf_counter() - t0)
            if rest > 0:
                time.sleep(rest)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()

    print(f"\nSent {sent}, valid replies {ok}")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
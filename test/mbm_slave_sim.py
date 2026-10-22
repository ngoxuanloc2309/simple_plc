#!/usr/bin/env python3
"""
mbm_slave_sim.py - Modbus RTU slave simulator, the sensor side of the MB_READ live test.

Run it on the PC on a USB-RS485 dongle wired A-A, B-B to the Zigbee-IO RS485
terminal, then run the live poll from another terminal:

    python mbm_slave_sim.py COM20 --unit 1 --baud 9600
    python test_mpm.py COM14 --live 1 --live-addr 0 --live-count 2

Answers FC01/02 (coils / discrete inputs), FC03/04 (holding / input registers)
and FC06/16 (writes to holding registers, for the later MB_WRITE test). Every
request and answer is printed in hex.

Register map (the same table for FC03 and FC04):
    0      1234                      fixed UINT16 (what test_mpm.py --live reads)
    1      5678                      fixed UINT16
    2      counter, +1 per request   proves that every poll is a new one
    3      0xFFFE                    INT16 -2
    4..5   REAL 23.5      (0x41BC 0x0000, High Word first)
    6..7   INT32 -12345   (0xFFFF 0xCFC7, High Word first)
    8..9   UINT32 100000  (0x0001 0x86A0, High Word first)
    10..   zeros; 0..99 are readable and writable (FC06/16), 100+ is exception 02
Coils / discrete inputs 0..15: bit n is set when n is even.

Fault injection (applies to EVERY answer), to see the error codes of the block:
    --fault timeout    never answer              -> err_code 1
    --fault crc        answer with a wrong CRC   -> err_code 2
    --fault exception  answer exception 0x04     -> err_code 3
    --fault short      answer with a cut frame   -> err_code 4 or 1
"""

import argparse
import struct
import sys
import time

REG_COUNT = 100
COIL_COUNT = 16


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(body: bytes) -> bytes:
    return body + struct.pack("<H", crc16(body))


class Slave:
    def __init__(self, unit: int, fault: str = "none"):
        self.unit = unit
        self.fault = fault
        self.requests = 0
        self.regs = [0] * REG_COUNT
        self.regs[0], self.regs[1] = 1234, 5678
        self.regs[3] = 0xFFFE
        self.regs[4], self.regs[5] = struct.unpack(">HH", struct.pack(">f", 23.5))
        self.regs[6], self.regs[7] = struct.unpack(">HH", struct.pack(">i", -12345))
        self.regs[8], self.regs[9] = struct.unpack(">HH", struct.pack(">I", 100000))

    def _exception(self, fc: int, code: int) -> bytes:
        return with_crc(bytes([self.unit, fc | 0x80, code]))

    def handle(self, frame: bytes):
        """Returns the bytes to send, or None for no answer."""
        if len(frame) < 4:
            return None
        if crc16(frame[:-2]) != struct.unpack("<H", frame[-2:])[0]:
            print("  ! bad CRC in the request, ignored")
            return None
        unit, fc = frame[0], frame[1]
        if unit != self.unit:
            return None                       # not for us (a real slave stays silent)
        self.requests += 1
        self.regs[2] = self.requests & 0xFFFF

        if self.fault == "timeout":
            return None
        if self.fault == "exception":
            return self._exception(fc, 0x04)

        rsp = self._answer(fc, frame[2:-2])

        if self.fault == "crc":
            rsp = rsp[:-1] + bytes([rsp[-1] ^ 0xFF])
        elif self.fault == "short":
            rsp = rsp[:max(3, len(rsp) - 4)]
        return rsp

    def _answer(self, fc: int, pdu: bytes) -> bytes:
        u = self.unit
        if fc in (1, 2):
            if len(pdu) != 4:
                return self._exception(fc, 3)
            addr, qty = struct.unpack(">HH", pdu)
            if not 1 <= qty <= 2000:
                return self._exception(fc, 3)
            if addr + qty > COIL_COUNT:
                return self._exception(fc, 2)
            nbytes = (qty + 7) // 8
            data = bytearray(nbytes)
            for i in range(qty):
                if (addr + i) % 2 == 0:
                    data[i // 8] |= 1 << (i % 8)
            return with_crc(bytes([u, fc, nbytes]) + bytes(data))

        if fc in (3, 4):
            if len(pdu) != 4:
                return self._exception(fc, 3)
            addr, qty = struct.unpack(">HH", pdu)
            if not 1 <= qty <= 125:
                return self._exception(fc, 3)
            if addr + qty > REG_COUNT:
                return self._exception(fc, 2)
            data = b"".join(struct.pack(">H", self.regs[addr + i]) for i in range(qty))
            return with_crc(bytes([u, fc, 2 * qty]) + data)

        if fc == 6:
            if len(pdu) != 4:
                return self._exception(fc, 3)
            addr, value = struct.unpack(">HH", pdu)
            if addr >= REG_COUNT:
                return self._exception(fc, 2)
            self.regs[addr] = value
            return with_crc(bytes([u, fc]) + pdu)

        if fc == 16:
            if len(pdu) < 5:
                return self._exception(fc, 3)
            addr, qty, nbytes = struct.unpack(">HHB", pdu[:5])
            if not 1 <= qty <= 123 or nbytes != 2 * qty or len(pdu) != 5 + nbytes:
                return self._exception(fc, 3)
            if addr + qty > REG_COUNT:
                return self._exception(fc, 2)
            for i in range(qty):
                self.regs[addr + i] = struct.unpack(">H", pdu[5 + 2 * i:7 + 2 * i])[0]
            return with_crc(bytes([u, fc]) + pdu[:4])

        return self._exception(fc, 1)


STALE_S = 0.3   # an incomplete request older than this is dropped


def request_len(buf):
    """Length of the request starting at buf[0], or None while it cannot be told yet."""
    if len(buf) < 2:
        return None
    if buf[1] in (15, 16):                    # unit fc addr(2) qty(2) bytes data crc(2)
        return None if len(buf) < 7 else 9 + buf[6]
    return 8                                  # FC01..06 (and unknown): fixed size


def hexs(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial port of the USB-RS485 dongle, e.g. COM20 or /dev/ttyUSB0")
    ap.add_argument("--unit", type=int, default=1, help="slave address (default 1)")
    ap.add_argument("--baud", type=int, default=9600, help="line speed; the firmware default is 9600")
    ap.add_argument("--parity", choices=["N", "O", "E"], default="N", help="default N")
    ap.add_argument("--stop", type=int, choices=[1, 2], default=1)
    ap.add_argument("--fault", choices=["none", "timeout", "crc", "exception", "short"], default="none")
    ap.add_argument("--delay-ms", type=float, default=0.0, help="wait this long before every answer")
    ap.add_argument("--trace", action="store_true",
                    help="print every chunk the port returns, with a timestamp, before any framing")
    ap.add_argument("--read-timeout-ms", type=float, default=5.0,
                    help="pyserial read timeout (raw_listen.py uses 1)")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial")
        return 2

    parity = {"N": serial.PARITY_NONE, "O": serial.PARITY_ODD, "E": serial.PARITY_EVEN}[args.parity]
    stop = serial.STOPBITS_ONE if args.stop == 1 else serial.STOPBITS_TWO
    # A USB-serial dongle hands bytes to the PC in batches (FTDI latency timer, ~16 ms), so
    # one request can arrive in two pieces. Ending a frame on a silence gap would split
    # such a request and lose it. Instead bytes are collected until a complete, CRC-valid
    # request is there (length from the function code).
    ser = serial.Serial(args.port, args.baud, bytesize=8, parity=parity, stopbits=stop,
                        timeout=args.read_timeout_ms / 1000.0)
    slave = Slave(args.unit, args.fault)
    print(f"slave {args.unit} on {args.port} at {args.baud} 8{args.parity}{args.stop}, "
          f"fault={args.fault}. Ctrl+C to stop.")
    buf = bytearray()
    last_rx = 0.0
    t0 = time.monotonic()
    try:
        while True:
            data = ser.read(256)
            now = time.monotonic()
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
                    break                      # wait for the rest of the frame
                if crc16(bytes(buf[:n - 2])) != struct.unpack("<H", bytes(buf[n - 2:n]))[0]:
                    print(f"  ! bad CRC, dropped byte {buf[0]:02X} to resynchronise")
                    del buf[0]
                    continue
                frame = bytes(buf[:n])
                del buf[:n]
                print(f"RX {hexs(frame)}")
                rsp = slave.handle(frame)
                if rsp is None:
                    continue
                if args.delay_ms > 0:
                    time.sleep(args.delay_ms / 1000.0)
                ser.write(rsp)
                ser.flush()
                print(f"TX {hexs(rsp)}")
    except KeyboardInterrupt:
        print(f"\nstopped after {slave.requests} request(s) for unit {args.unit}")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
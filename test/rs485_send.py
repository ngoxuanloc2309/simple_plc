#!/usr/bin/env python3
"""
rs485_send.py - send bytes to the board's RS485 test (TEST_RS485=1 in Core/Src/main.c) and
print everything that comes back. Needs: pip install pyserial

Examples
  python rs485_send.py COM5                      # stream 'U' (0x55) x64 continuously
  python rs485_send.py COM5 --mode text          # "ipconfig\r\n" once a second
  python rs485_send.py COM5 --mode text --text "hello" --interval 0.5
  python rs485_send.py COM5 --mode hex --hex "01 03 00 00 00 02 C4 0B"
  python rs485_send.py COM5 --list               # list serial ports and exit

Stream mode (default) is what the firmware's "[RS485 PROBE]" window needs: continuous 0x55
makes PB15 toggle, so 'edges in 50 ms' must jump from 0 to a large number.
Stop with Ctrl+C.
"""
import argparse
import sys
import time

import serial
import serial.tools.list_ports


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200, help="default 115200 (USART1 in CubeMX)")
    p.add_argument("--mode", choices=["stream", "text", "hex"], default="stream",
                   help="stream = 0x55 x chunk back to back; text = --text once per --interval; "
                        "hex = --hex once per --interval")
    p.add_argument("--chunk", type=int, default=64, help="stream mode: bytes per write (default 64)")
    p.add_argument("--text", default="ipconfig", help="text mode payload (\\r\\n is appended)")
    p.add_argument("--hex", default="55 55 55 55", help="hex mode payload, e.g. '01 03 00 00 00 02 C4 0B'")
    p.add_argument("--interval", type=float, default=1.0, help="seconds between frames (text/hex mode)")
    p.add_argument("--rts", choices=["off", "on", "toggle"], default="off",
                   help="RTS line: off/on for adapters that need RTS high to transmit; "
                        "toggle = RTS high while writing, low after (adapters with RTS-driven DE)")
    p.add_argument("--dtr", choices=["off", "on"], default="off", help="DTR line level")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    return p.parse_args()


def hexdump(data: bytes) -> str:
    txt = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in data)
    return f"{len(data)} B: {' '.join(f'{b:02X}' for b in data)} | \"{txt}\""


def main():
    a = parse_args()
    if a.list:
        for port in serial.tools.list_ports.comports():
            print(f"{port.device}\t{port.description}")
        return 0
    if not a.port:
        print("error: give a port (use --list to see them)", file=sys.stderr)
        return 2

    if a.mode == "stream":
        payload = b"U" * max(1, a.chunk)
    elif a.mode == "text":
        payload = a.text.encode("ascii", "replace") + b"\r\n"
    else:
        try:
            payload = bytes.fromhex(a.hex)
        except ValueError:
            print("error: --hex must be hex bytes, e.g. '01 03 00 00'", file=sys.stderr)
            return 2

    # serial_for_url accepts real ports (COM5, /dev/ttyUSB0) and test URLs (loop://)
    ser = serial.serial_for_url(a.port, baudrate=a.baud, bytesize=8, parity=serial.PARITY_NONE,
                                stopbits=1, timeout=0, write_timeout=2)
    ser.dtr = (a.dtr == "on")
    ser.rts = (a.rts == "on")
    print(f"{a.port} {a.baud} 8N1, mode={a.mode}, payload {len(payload)} B, rts={a.rts}. Ctrl+C to stop.")

    sent_total = 0
    last_tx = 0.0
    rx_buf = bytearray()
    rx_last = 0.0
    try:
        while True:
            now = time.monotonic()
            if a.mode == "stream" or now - last_tx >= a.interval:
                if a.rts == "toggle":
                    ser.rts = True
                ser.write(payload)
                if a.rts == "toggle":
                    ser.flush()
                    ser.rts = False
                sent_total += len(payload)
                last_tx = now
                if a.mode != "stream":
                    print(f"TX {hexdump(payload)}")

            data = ser.read(4096)
            if data:
                rx_buf += data
                rx_last = now
            # print a received frame once the line has been quiet for 30 ms
            if rx_buf and now - rx_last > 0.03:
                print(f"RX {hexdump(bytes(rx_buf))}")
                rx_buf.clear()

            if a.mode == "stream":
                # one write at a time; the OS buffer paces us at the line rate
                if sent_total % (a.chunk * 200) < a.chunk:
                    print(f"sent {sent_total} bytes", end="\r", flush=True)
            else:
                time.sleep(0.005)
    except KeyboardInterrupt:
        print(f"\nstopped, sent {sent_total} bytes")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
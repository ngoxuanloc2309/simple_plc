#!/usr/bin/env python3
"""
rawlisten.py - print every byte that arrives on a serial port, with timestamps.

Use it INSTEAD of mbm_slave_sim.py to see what the board really puts on the RS485
bus (complete frames, cut frames, garbage). It never answers.

    python rawlisten.py COM35 9600
    python rawlisten.py COM35 9600 --parity E --gap-ms 5

A new line starts after --gap-ms of silence (default 5 ms), so one line = one
frame. A good MB_READ request is 8 bytes: unit fc addr(2) count(2) crc(2),
for example 01 03 00 00 00 02 C4 0B.
"""
import argparse
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("baud", type=int, nargs="?", default=9600)
    ap.add_argument("--parity", choices=["N", "E", "O"], default="N")
    ap.add_argument("--stop", type=int, choices=[1, 2], default=1)
    ap.add_argument("--gap-ms", type=float, default=5.0, help="silence that ends a frame (default 5 ms)")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial")
        return 2

    parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[args.parity]
    stop = serial.STOPBITS_ONE if args.stop == 1 else serial.STOPBITS_TWO
    ser = serial.Serial(args.port, args.baud, bytesize=8, parity=parity, stopbits=stop, timeout=0.001)
    print(f"listening on {args.port} at {args.baud} 8{args.parity}{args.stop}, "
          f"frame gap {args.gap_ms} ms. Ctrl+C to stop.")

    t0 = time.perf_counter()
    frame = bytearray()
    first = 0.0
    last = 0.0
    count = 0
    try:
        while True:
            data = ser.read(256)
            now = time.perf_counter()
            if data:
                if not frame:
                    first = now
                frame += data
                last = now
            elif frame and (now - last) * 1000.0 >= args.gap_ms:
                count += 1
                span = (last - first) * 1000.0
                print(f"{first - t0:9.3f}s  n={len(frame):3d}  span={span:6.2f} ms  "
                      f"{' '.join(f'{b:02X}' for b in frame)}")
                frame.clear()
    except KeyboardInterrupt:
        print(f"\nstopped, {count} frame(s) seen")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
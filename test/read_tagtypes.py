#!/usr/bin/env python3
"""Read-only dump of the Tag DataType table (0x0080..0x00BF).

Writes NOTHING and touches no Flash. Safe on any board.

  python read_tagtypes.py COM17 --unit 1 --baudrate 115200
  python read_tagtypes.py COM17 --tags 40,41,42,43      # show only these tags

Byte per tag: 0 = not declared (treated as INT32), 1 = BOOL, 2 = INT32, 3 = REAL.
Tag 2k is the HIGH byte of register 0x0080+k, tag 2k+1 the LOW byte.
Put this file in the same folder as test_mqtt.py (it reuses its helpers).
"""
import argparse
import sys

import test_mqtt as T

REG_TYPES = 0x0080
TYPES_REGS = 64
NAMES = {0: "undeclared(=INT32)", 1: "BOOL", 2: "INT32", 3: "REAL"}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--tags", default=None, help="comma list of tag indexes to show (default: every non-zero)")
    p.add_argument("--tel", action="store_true",
                   help="also read the 4 MQTT telemetry blocks (0x0C00 + i*96) and show the tags each one is bound to")
    args = p.parse_args()

    client = T.make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    d = T.Dev(client, args.unit)
    tel = []
    try:
        regs = d.read(REG_TYPES, TYPES_REGS)
        if args.tel:
            for i in range(4):
                base = 0x0C00 + i * 96
                hdr = d.read(base, 16)          # +0 control, +6 tag_count, +7..+14 tag indexes
                tel.append((i, hdr[0], hdr[6], hdr[7:15]))
    finally:
        client.close()

    img = bytearray()
    for r in regs:
        img += bytes([(r >> 8) & 0xFF, r & 0xFF])

    if args.tags:
        wanted = [int(x) for x in args.tags.split(",")]
    else:
        wanted = [i for i, b in enumerate(img) if b != 0]

    for i, ctrl, cnt, tags in tel:
        shown = [t for t in tags[:cnt]]
        print(f"telemetry block {i}: control=0x{ctrl:04X} tag_count={cnt} tags={shown}")
    print("raw registers 0x0080..: " + " ".join(f"{r:04X}" for r in regs))
    if tel:
        bound = sorted({t for _, _, cnt, tags in tel for t in tags[:cnt] if t != 0xFFFF})
        wanted = sorted(set(wanted) | set(bound))
    if not wanted:
        print("table is all zero: no tag is declared, every tag prints as INT32")
        return
    for t in wanted:
        b = img[t] if t < len(img) else 0
        print(f"tag {t:3d}: {b} = {NAMES.get(b, '?')}")


if __name__ == "__main__":
    main()
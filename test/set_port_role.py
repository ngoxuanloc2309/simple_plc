#!/usr/bin/env python3
"""
set_port_role.py - put an RS-485 port into the Slave (or back into the Master) role through the
board's USB-CDC App link, the way the App does it (port table 0x1400, SPLC-SPEC-MBM-003).
Prepares the board for test_mbs.py (plc_mbs, decision 75 in docs/handoff.md).

Run on the board's USB-CDC port (COM14), NOT on the RS-485 dongle:

    python set_port_role.py COM14 --slave-id 1 --baud 9600           # port 1 -> Slave
    python set_port_role.py COM14 --master --baud 9600               # port 1 -> Master again
    python set_port_role.py COM14 --check                            # only report

then, on the RS-485 dongle:

    python test_mbs.py COM35 --unit 1 --baudrate 9600

What it does
  1 reads 0x0A08 and tells whether the firmware announces MODBUS_SLAVE (0x4000), i.e. was built
    with SPLC_MODBUS_SLAVE_ENABLE 1. Without it the port is still configured but nobody answers.
  2 writes the record of the port into the draft: +0 = enabled | port | bit 9 (Slave),
    +14 = PORT_ROLE, +15 = Slave ID. For a Slave, +5/+6/+7 are written with the defaults the
    App writes (they are ignored by the firmware).
  3 writes the 4 MB_READ blocks DISABLED into the same draft (a block that points at a Slave
    port is refused at COMMIT with error 7), then COMMIT.
  4 reads 0x1400 back and checks role / Slave ID / baud.

WARNING: the COMMIT replaces the active rule table with ONE harmless rule (INTERVAL 60 s,
INC_COUNTER on a VREG) and disables the MB_READ blocks, exactly like test_mbm_live.py does.
It writes Flash. Dev board only. The role is stored in Flash and survives a reboot. CLEAR_RULES
and FACTORY_RESET erase it (they write an empty record): to go back, use --master.
Zigbee-IO has ONE RS-485 port (USART1): while it is a Slave, test_mbm_live.py cannot run.
Exit codes: 0 = OK, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""
import argparse
import sys

import test_mqtt as T
import test_mpm as M
from test_mqtt import check, info, LinkError

REG_CAPS = 0x0A08
CAP_MODBUS_SLAVE = 0x4000
ROLE_MASTER, ROLE_SLAVE = 0, 1


def show_table(d, port_no):
    t = d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS)
    b = (port_no - 1) * M.PORT_REGS
    rec = t[b:b + M.PORT_REGS]
    baud = (rec[1] << 16) | rec[2]
    par = {0: "N", 1: "E", 2: "O"}.get(rec[3], "?")
    info(f"port {port_no}: +0=0x{rec[0]:04X} (enabled={rec[0] >> 8 & 1}, bit9={rec[0] >> 9 & 1}, "
         f"port={rec[0] & 0xFF})  baud={baud} {par}  +14 role={rec[14]}  +15 slave_id={rec[15]}")
    return rec, baud


def run(d, a):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain = r[3], r[4], r[5], r[6], r[7], r[8]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} RETAIN={retain}")
    caps_regs, err = d.try_read(REG_CAPS, 2)
    caps = ((caps_regs[0] << 16) | caps_regs[1]) if err is None else 0
    info(f"0x0A08 capabilities = 0x{caps:08X}")
    has_slave = bool(caps & CAP_MODBUS_SLAVE)
    if a.check:
        show_table(d, a.rs485_port)
        info("firmware announces MODBUS_SLAVE" if has_slave else
             "firmware does NOT announce MODBUS_SLAVE (0x4000): build with SPLC_MODBUS_SLAVE_ENABLE 1")
        return
    if not a.master:
        if not has_slave:
            check(False, "firmware announces MODBUS_SLAVE (0x4000)")
            info("build with SPLC_MODBUS_SLAVE_ENABLE 1 (needs SPLC_MBM_ENABLE 1), flash, run again")
            return
        check(True, "firmware announces MODBUS_SLAVE (0x4000)")

    port_no = a.rs485_port
    n, err = d.try_read(M.REG_PORTS, 1)
    if err is None:
        if not check(port_no <= n[0], f"the device has RS485 port {port_no} (it has {n[0]})"):
            return

    if a.master:
        if not M.write_port_table(d, port_no, a.baud, a.parity, a.stop):
            return
    else:
        table = d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS)
        new, b = list(table), (port_no - 1) * M.PORT_REGS
        new[b + 0] = 0x0100 | 0x0200 | port_no            # enabled | Slave (bit 9) | port number
        new[b + 1], new[b + 2] = a.baud >> 16, a.baud & 0xFFFF
        new[b + 3] = M.PARITY[a.parity]
        new[b + 4] = (8 << 8) | a.stop
        new[b + 5], new[b + 6], new[b + 7] = 200, 20, 1   # ignored for a Slave, App writes these
        new[b + 14], new[b + 15] = ROLE_SLAVE, a.slave_id
        resp = d.write_regs(M.REG_PORT_TABLE, new)
        if not check(resp is None, f"port table: port {port_no} Slave id {a.slave_id} "
                                   f"{a.baud} {a.parity}{a.stop} (draft)"):
            return

    # No Master block may point at a Slave port: write all four disabled into the same draft.
    resp = M.write_blocks(d, {i: M.block(enabled=False) for i in range(4)})
    check(resp is None, "4 MB_READ blocks written disabled (draft)")

    di0, do0 = 0, di
    vf0 = di + do + ai
    vr0 = vf0 + vflag
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)
    s, e = T.commit(d, [rule])
    ok = check(s == T.CFG_READY and e == 0, f"COMMIT ok (status {s}, error {e})")
    if not ok:
        info("error 7 = a Master block points at the Slave port; error 2 = a field was refused")
        return

    rec, baud = show_table(d, port_no)
    want_role = ROLE_MASTER if a.master else ROLE_SLAVE
    check(rec[14] == want_role, f"+14 PORT_ROLE reads back {want_role}")
    if not a.master:
        check(rec[15] == a.slave_id, f"+15 Slave ID reads back {a.slave_id}")
        check(rec[0] >> 9 & 1 == 1, "+0 bit 9 (Slave) is set")
    check(baud == a.baud, f"baud reads back {a.baud}")
    if not a.master:
        info(f"now run:  python test_mbs.py <RS485 dongle> --unit {a.slave_id} --baudrate {a.baud}"
             f" --parity {a.parity}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="board USB-CDC port, e.g. COM14 (NOT the RS-485 dongle)")
    p.add_argument("--unit", type=int, default=1, help="Modbus unit of the App link (default 1)")
    p.add_argument("--baudrate", type=int, default=115200, help="App link baud (default 115200)")
    p.add_argument("--rs485-port", type=int, choices=[1, 2, 3, 4], default=1,
                   help="RS485 port to change (default 1, the only one of Zigbee-IO)")
    p.add_argument("--slave-id", type=int, default=1, help="Slave ID 1..247 (default 1)")
    p.add_argument("--baud", type=int, default=9600, help="RS485 line speed (default 9600)")
    p.add_argument("--parity", choices=["N", "E", "O"], default="N")
    p.add_argument("--stop", type=int, choices=[1, 2], default=1)
    p.add_argument("--master", action="store_true", help="put the port back into the Master role")
    p.add_argument("--check", action="store_true", help="only report the capability and the port record")
    a = p.parse_args()
    if not 1 <= a.slave_id <= 247:
        p.error("--slave-id must be 1..247")
    client = T.make_client(a.port, a.baudrate)
    if not client.connect():
        print(f"Cannot open {a.port}")
        sys.exit(2)
    d = T.Dev(client, a.unit)
    try:
        run(d, a)
    except LinkError as e:
        print(f"\n  [LINK] {e}")
        T.cleanup_after_abort(d)
        sys.exit(3)
    finally:
        try:
            d.c.close()
        except Exception:
            pass
    print("\nOK" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
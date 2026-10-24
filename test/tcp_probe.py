#!/usr/bin/env python3
"""
tcp_probe.py - READ-ONLY check of the SimplePLC Modbus TCP link (gateway port 5020).
Writes nothing, touches no Flash. Safe on any board.

    python tcp_probe.py 192.168.1.50 [--port 5020] [--unit 1]

Expected on a gateway built with SPLC_MODBUS_MAX_TRANSPORTS 2 + SPLC_HOST_MODBUS_TCP_ENABLE 1:
  * every read below succeeds, the same values as over RS485/USB
  * capabilities (0x0A08) contain 0x0400 (MODBUS_TCP; announced once the TCP transport is registered)
Failure hints:
  cannot connect   -> wrong IP/port, TCP build option off, or your IP is outside ALLOW_NET/_MASK
                      (gateway log: "peer x.x.x.x rejected"); look for "listening on port 5020"
  connects, no reply -> check --unit (SPLC_MODBUS_UNIT_ID) and the gateway log
"""
import argparse
import inspect
import sys

from pymodbus.client import ModbusTcpClient


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("--port", type=int, default=5020)
    p.add_argument("--unit", type=int, default=1)
    a = p.parse_args()

    c = ModbusTcpClient(a.host, port=a.port, timeout=3.0)
    if not c.connect():
        print(f"cannot connect to {a.host}:{a.port} (IP? port? TCP option off? ALLOW_NET filter?)")
        return 1
    params = inspect.signature(c.read_holding_registers).parameters
    kw = {"device_id": a.unit} if "device_id" in params else {"slave": a.unit}

    def rd(addr, n):
        try:
            r = c.read_holding_registers(address=addr, count=n, **kw)
        except Exception as e:
            return None, str(e)
        if r.isError():
            return None, f"exception {getattr(r, 'exception_code', '?')}"
        return list(r.registers), None

    ok = True
    for name, addr, n in (("descriptor", 0x0000, 10), ("rule table info", 0x0010, 1),
                          ("resource info", 0x0020, 10), ("health", 0x0800, 10)):
        regs, err = rd(addr, n)
        if err:
            ok = False
            print(f"  [FAIL] {name:16s} 0x{addr:04X}: {err}")
        else:
            print(f"  [ OK ] {name:16s} 0x{addr:04X}: {[hex(x) for x in regs]}")

    regs, err = rd(0x0A08, 2)
    if err:
        print(f"  [info] capabilities 0x0A08: {err}")
    else:
        caps = (regs[0] << 16) | regs[1]
        has_tcp = bool(caps & 0x0400)
        ok = ok and has_tcp
        print(f"  [{' OK ' if has_tcp else 'FAIL'}] capabilities 0x0A08 = 0x{caps:08X}"
              f"  MODBUS_TCP(0x0400)={'yes' if has_tcp else 'NO'}"
              f"  WIFI(0x200)={'yes' if caps & 0x200 else 'no'}  REAL(0x800)={'yes' if caps & 0x800 else 'no'}"
              f"  MQTT_SUB(0x20)={'yes' if caps & 0x20 else 'no'}")
    c.close()
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
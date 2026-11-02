#!/usr/bin/env python3
"""
test_port_role.py - RS-485 port ROLE (SPLC-SPEC-SLAVE-001) through the Wire Profile V2 port table
(0x1400) and the COMMIT rules that depend on it. Over the App link (USB-CDC), like test_mpm.py.

What it covers
  1. Role is decided by +14 ALONE (decision 76). Bit 9 of +0 is deprecated and ignored:
       +14 = 1 (Slave), bit 9 = 0 or 1, id 5          -> COMMIT READY
       +14 = 0 (Master), bit 9 = 1 (contradiction)    -> COMMIT READY (it used to be rejected)
       +14 = 0 with a stray +15 = 77                  -> COMMIT READY (a Master's +15 is ignored)
  2. Fields that must be refused (COMMIT status 4, CONFIG_ERROR_CODE 2 = INVALID_PARAMETER):
       +14 = 2, +14 = 255, Slave with id 0, Slave with id 248   (ids 1 and 247 are accepted)
     A refused COMMIT is all-or-nothing: the running port record does not change.
  3. Bit 9 really is ignored, observed through behaviour (not just "COMMIT accepted"):
       a port with +14 = 0 and bit 9 = 1 still takes an MB_READ block      (it is a Master)
       a port with +14 = 1 and bit 9 = 0 refuses an MB_READ block with 7   (it is a Slave)
  4. CONFIG_ERROR_CODE 7 (SLAVE_PORT_CONFLICT), CONFIG_STATUS 4:
       an enabled MB_READ block naming an enabled Slave port         -> 4 / 7
       an enabled MB_WRITE block naming it (when MB_WRITE is built)  -> 4 / 7
       the same COMMIT that turns the port into a Slave AND names it -> 4 / 7, port stays Master
       a DISABLED block that only carries the port number            -> READY (not a conflict)
     After a refused COMMIT the active rule version is unchanged.
  5. Cleanup: the port table you started with is written back, every block is disabled and the
     rule table is empty.

Needs: firmware with SPLC_MBM_ENABLE=1 and SPLC_MODBUS_SLAVE_ENABLE=1 (MB_WRITE checks run only
if SPLC_MBW_ENABLE=1). Only the USB-CDC link is used; no RS-485 dongle is needed. If you also run
test_mbs.py right after, the Slave port is back as you left it (the saved table is restored).

WARNING: WRITES Flash (port table, blocks, rule table) and ends with CLEAR_RULES: rules and MB
blocks already on the board are lost. Dev board only.

Usage:
    python test_port_role.py COM14
    python test_port_role.py COM14 --rs485-port 3

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""
import argparse
import sys

import test_report as R   # Excel evidence report (--report / --no-report)
import test_mqtt as T
import test_mpm as M
import test_mbw as W
from test_mqtt import check, info, LinkError

CFG_ERROR = 4          # CONFIG_STATUS_ERROR (plc_modbus_cfg.c)
ERR_SLAVE_PORT_CONFLICT = 7
PF = 16                # registers per port record
PF_CONTROL, PF_ROLE, PF_ID = 0, 14, 15
BIT9 = 0x0200


# ------------------------------------------------------------------ helpers
def port_image(base, port_no, role, slave_id, bit9=False):
    """Port table (64 registers) = `base` with the record of `port_no` enabled and carrying
    this role. The line settings already in the record are kept (9600 8N1 if it was blank)."""
    t, b = list(base), (port_no - 1) * PF
    t[b + PF_CONTROL] = 0x0100 | port_no | (BIT9 if bit9 else 0)
    if t[b + 1] == 0 and t[b + 2] == 0:
        t[b + 1], t[b + 2] = 0, 9600
        t[b + 3] = 0
        t[b + 4] = (8 << 8) | 1
        t[b + 5], t[b + 6], t[b + 7] = 200, 20, 1
    t[b + PF_ROLE], t[b + PF_ID] = role, slave_id
    return t


def record(table, port_no):
    return table[(port_no - 1) * PF:port_no * PF]


def put_table(d, image):
    return d.write_regs(M.REG_PORT_TABLE, image)


def version(d):
    return d.read(T.REG_VERSION, 1)[0]


def commit(d, rule):
    return T.commit(d, [rule])


def expect_ready(d, rule, what):
    s, e = commit(d, rule)
    return check(s == T.CFG_READY and e == 0, f"{what}: COMMIT accepted (status {s}, error {e})")


def expect_rejected(d, rule, what, err):
    v0 = version(d)
    s, e = commit(d, rule)
    ok = check(s == CFG_ERROR and e == err,
               f"{what}: COMMIT refused with status {CFG_ERROR} / error {err} (got {s} / {e})")
    check(version(d) == v0, f"{what}: the active rule version did not move")
    return ok


def set_role(d, rule, orig, port_no, role, sid, bit9, what):
    """Draft the port record, COMMIT. Returns (status, error)."""
    resp = put_table(d, port_image(orig, port_no, role, sid, bit9))
    check(resp is None, f"{what}: port table draft write ACKed (content is checked at COMMIT)")
    return commit(d, rule)


def read_block(port_idx, vr1, slave=1):
    """A valid enabled MB_READ block (one INT16 register into a plain VREG) on RS-485 port
    index `port_idx` (0..3)."""
    return M.block(enabled=True, fc=3, slave=slave, start=0, count=1, interval=500, timeout=200,
                   pins=[M.pin(tag=vr1, off=0, dt=M.DT_INT16)], port=port_idx)


def write_block(port_idx, vr1):
    return W.block(fc=16, slave=3, start=0x50, count=1, interval=1000, timeout=300, trig_mode=1,
                   port=port_idx, pins=[W.pin(vr1, 0, W.DT_INT16)])


def enabled_blocks(img, regs_per_block):
    """Indices of the blocks whose enabled bit (bit 0 of register 0) is set in a block-area image."""
    return [i for i in range(len(img) // regs_per_block) if img[i * regs_per_block] & 1]


def disabled_block_with_port(port_idx):
    b = [0] * M.BLOCK_REGS
    b[0] = (port_idx & 0xF) << 4          # enabled bit 0 stays 0
    return b


# ---------------------------------------------------------------------- run
def run(d, args):
    port_no = args.rs485_port
    pidx = port_no - 1

    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg = r[3], r[4], r[5], r[6], r[7]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg}")
    if vreg < 4:
        print("Need >= 4 VREG tags")
        return
    vr0 = di + do + ai + vflag
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)

    # ---------------------------------------------------------------- 0
    print("\n0. Probe")
    caps, cerr = d.try_read(M.REG_CAPS, 2)
    if cerr is None:
        v = (caps[0] << 16) | caps[1]
        if not check(v & M.CAP_MODBUS_MASTER != 0, f"0x0A08 announces MODBUS_MASTER (caps 0x{v:08X})"):
            print("  Build with SPLC_MBM_ENABLE 1, clean rebuild, flash. Stopping.")
            return
    n, perr = d.try_read(M.REG_PORTS, 1)
    if perr is not None or not check(port_no <= n[0], f"the device has RS485 port {port_no} (count {n})"):
        return
    orig = d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS)
    rec = record(orig, port_no)
    print(f"  saved port table; record {port_no} = control 0x{rec[PF_CONTROL]:04X} role {rec[PF_ROLE]} "
          f"id {rec[PF_ID]} baud {(rec[1] << 16) | rec[2]}")
    has_mbw = d.write_regs(W.REG_MBW, [0]) is None       # a draft write; the COMMIT below drops it
    info("MB_WRITE (0x1100) is built" if has_mbw else "MB_WRITE (0x1100) not built: its checks are skipped")
    expect_ready(d, rule, "baseline COMMIT (drops the probe draft)")

    print("\n1. Clean start: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")

    try:
        # ------------------------------------------------------------ 2
        print("\n2. The role is +14 alone: bit 9 of +0 is ignored")
        s, e = set_role(d, rule, orig, port_no, 1, 5, False, "Slave, id 5, bit 9 = 0")
        check(s == T.CFG_READY and e == 0, f"Slave id 5, bit 9 = 0: COMMIT accepted (status {s}, error {e})")
        s, e = set_role(d, rule, orig, port_no, 1, 5, True, "Slave, id 5, bit 9 = 1")
        check(s == T.CFG_READY and e == 0, f"Slave id 5, bit 9 = 1: COMMIT accepted (status {s}, error {e})")
        s, e = set_role(d, rule, orig, port_no, 0, 0, True, "Master, bit 9 = 1")
        check(s == T.CFG_READY and e == 0,
              f"Master (+14 = 0) with bit 9 = 1: COMMIT accepted (status {s}, error {e}); it used to be refused")
        s, e = set_role(d, rule, orig, port_no, 0, 77, False, "Master, stray +15")
        check(s == T.CFG_READY and e == 0,
              f"Master with a stray +15 = 77: COMMIT accepted (status {s}, error {e}); a Master's +15 is ignored")
        good = d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS)

        # ------------------------------------------------------------ 3
        print("\n3. Fields that must be refused (error 2), and the port record stays as it was")
        cases = [("+14 = 2", 2, 5), ("+14 = 255", 255, 5), ("Slave with id 0", 1, 0), ("Slave with id 248", 1, 248)]
        for name, role, sid in cases:
            v0 = version(d)
            s, e = set_role(d, rule, good, port_no, role, sid, False, name)
            check(s == CFG_ERROR and e == T.ERR_INVALID_PARAMETER,
                  f"{name}: COMMIT refused with status {CFG_ERROR} / error 2 (got {s} / {e})")
            check(version(d) == v0, f"{name}: the active rule version did not move")
            check(d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS) == good,
                  f"{name}: the running port table is unchanged (all-or-nothing)")
        for name, sid in (("Slave with id 1", 1), ("Slave with id 247", 247)):
            s, e = set_role(d, rule, good, port_no, 1, sid, False, name)
            check(s == T.CFG_READY and e == 0, f"{name}: COMMIT accepted (status {s}, error {e})")

        # ------------------------------------------------------------ 4
        print("\n4. Bit 9 is ignored in behaviour: the role of +14 decides who may poll")
        s, e = set_role(d, rule, good, port_no, 0, 0, True, "Master + bit 9")
        check(s == T.CFG_READY and e == 0, f"port {port_no} = Master with bit 9 = 1 (status {s}, error {e})")
        resp = M.write_blocks(d, {0: read_block(pidx, vr0 + 1)})
        check(resp is None, "MB_READ block on that port: draft write ACKed")
        expect_ready(d, rule, "MB_READ block on a Master port with bit 9 = 1 (bit 9 did not make it a Slave)")
        s, e = set_role(d, rule, good, port_no, 1, 5, False, "Slave, bit 9 = 0")
        check(s == CFG_ERROR and e == ERR_SLAVE_PORT_CONFLICT,
              f"turning the port into a Slave (bit 9 = 0) while block 0 still polls it: "
              f"refused with 4 / 7 (got {s} / {e})")
        info("that block is still enabled in the RUNNING config, so the COMMIT above is refused as a whole")

        # ------------------------------------------------------------ 5
        print("\n5. Error 7: an enabled MB_READ block naming an enabled Slave port")
        # Start from a clean running config: Master port, no blocks.
        T.run_syscmd(d, T.SYS_CLEAR_RULES)                       # disables every block
        s, e = set_role(d, rule, good, port_no, 1, 5, False, "Slave")
        check(s == T.CFG_READY and e == 0, f"port {port_no} = Slave id 5 (status {s}, error {e})")
        check(M.write_blocks(d, {0: read_block(pidx, vr0 + 1)}) is None, "MB_READ block 0 on the Slave port: draft ACKed")
        expect_rejected(d, rule, "MB_READ on a Slave port", ERR_SLAVE_PORT_CONFLICT)
        check(not enabled_blocks(M.read_mbm(d), M.BLOCK_REGS), "the refused draft left no MB_READ block running")

        print("\n   a DISABLED block that only carries the port number is not a conflict")
        check(M.write_blocks(d, {1: disabled_block_with_port(pidx)}) is None, "disabled block 1: draft ACKed")
        expect_ready(d, rule, "disabled block naming the Slave port")

        print("\n   the same COMMIT turns the port into a Slave and names it: refused, port stays Master")
        T.run_syscmd(d, T.SYS_CLEAR_RULES)
        s, e = set_role(d, rule, good, port_no, 0, 0, False, "Master")
        check(s == T.CFG_READY and e == 0, f"port {port_no} = Master again (status {s}, error {e})")
        before = d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS)
        check(put_table(d, port_image(good, port_no, 1, 5, False)) is None, "draft: port becomes a Slave")
        check(M.write_blocks(d, {0: read_block(pidx, vr0 + 1)}) is None, "draft: block 0 polls that port")
        expect_rejected(d, rule, "same-COMMIT conflict (port becomes Slave + block names it)", ERR_SLAVE_PORT_CONFLICT)
        check(d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS) == before,
              "the port record is still the Master one (all-or-nothing)")
        check(record(d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS), port_no)[PF_ROLE] == 0, "+14 still reads 0")

        # ------------------------------------------------------------ 6
        if has_mbw:
            print("\n6. Error 7: an enabled MB_WRITE block naming an enabled Slave port")
            T.run_syscmd(d, T.SYS_CLEAR_RULES)
            s, e = set_role(d, rule, good, port_no, 1, 5, False, "Slave")
            check(s == T.CFG_READY and e == 0, f"port {port_no} = Slave id 5 (status {s}, error {e})")
            check(W.write_blocks(d, {0: write_block(pidx, vr0 + 1)}) is None, "MB_WRITE block 0 on the Slave port: draft ACKed")
            expect_rejected(d, rule, "MB_WRITE on a Slave port", ERR_SLAVE_PORT_CONFLICT)
            check(not enabled_blocks(W.read_mbw(d), W.BLOCK_REGS), "the refused draft left no MB_WRITE block running")
            T.run_syscmd(d, T.SYS_CLEAR_RULES)
            s, e = set_role(d, rule, good, port_no, 0, 0, False, "Master")
            check(s == T.CFG_READY and e == 0, f"port {port_no} = Master (status {s}, error {e})")
            check(W.write_blocks(d, {0: write_block(pidx, vr0 + 1)}) is None, "MB_WRITE block 0 on the Master port: draft ACKed")
            expect_ready(d, rule, "MB_WRITE on a Master port")
        else:
            print("\n6. skipped (no MB_WRITE in this build)")
    finally:
        # ------------------------------------------------------------ 7
        print("\n7. Cleanup: the port table you started with, no blocks, no rules")
        try:
            T.run_syscmd(d, T.SYS_CLEAR_RULES)                    # every block disabled
            check(put_table(d, orig) is None, "original port table: draft written")
            expect_ready(d, rule, "restore COMMIT")
            check(d.read(M.REG_PORT_TABLE, M.PORT_TABLE_REGS) == orig, "the port table reads as it did at the start")
            s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
            check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
        except LinkError:
            raise
        except Exception as ex:                                    # keep the original error visible
            print(f"  (cleanup) FAILED ({ex}): check the port table on the board")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--rs485-port", type=int, choices=[1, 2, 3, 4], default=3,
                   help="RS485 port whose role is changed (default 3)")
    p.add_argument("--chunk", type=int, default=48, help="registers per FC16 (default 48)")
    R.add_args(p)
    args = p.parse_args()
    R.start(args)
    T.Dev.chunk = max(1, min(args.chunk, 120))

    client = T.make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    d = T.Dev(client, args.unit)
    link_error = None
    try:
        run(d, args)
    except LinkError as e:
        link_error = e
        print(f"\n  [LINK] {e}")
        T.cleanup_after_abort(d)
    except Exception:
        import traceback
        traceback.print_exc()
        print("\n  [ERROR] unexpected exception in the test script (not a firmware verdict)")
        T.cleanup_after_abort(d)
        T._failures += 1
    finally:
        try:
            d.c.close()
        except Exception:
            pass
    if link_error is not None:
        print("\nINCONCLUSIVE: a request outcome was unknown (link error), NOT a firmware failure."
              f" {T._failures} check(s) failed before it.")
        sys.exit(3)
    print("\nALL PASS" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
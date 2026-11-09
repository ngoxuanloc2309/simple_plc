#!/usr/bin/env python3
"""
test_zigbee_cfg.py - Zigbee port block 0x1700..0x173F (SPLC-SPEC-ZIGBEE-006 v0.2, decision 98)
and the MB_READ / MB_WRITE rules that name the Zigbee pipe (raw port field 4). Over the App link
(USB-CDC), like test_port_role.py. No Zigbee module and no RS-485 dongle needed: only the
configuration path is tested, NOT bytes on the radio.

What it covers
  1. Layout: 64 registers read; +37..+63 read 0; NET_KEY (+5..+12) reads 0; PRESERVE_KEY (bit 5)
     and bits 6..15 of CONTROL_FLAGS read 0; STATUS_FLAGS bit 0 = KEY_SET.
  2. Read-only part: a write to +32..+63 is ACKed and ignored; a write that runs past 0x173F or
     starts below 0x1700 is refused with Modbus exception 0x02.
  3. A COMMIT that does not write the block keeps the running configuration.
  4. Disabled configuration is stored and read back verbatim (+0..+4, +13..+31 incl. reserved).
  5. COMMIT refusals (status 4 / error 2), all-or-nothing, and the draft is DROPPED after a
     refused COMMIT: FRAMING = 1, bits 6..15, bit 5 PRESERVE_KEY is never stored.
  6. A partial write (only SLAVE_ID) keeps the rest of the block.
  7. Network key rules (spec section 5), only the ones that can be checked: SECURITY without a new
     key, with an all-zero key, PRESERVE_KEY with no key stored; a stored key reads 0 but
     KEY_SET = 1; a partial write while SECURITY is running needs PRESERVE_KEY; SECURITY = 0 keeps
     the stored key (firmware assumption 9 of the spec).
  8. Port dependent rules: no port registered (capability 0x8000 absent) => ENABLE = 1 is refused
     for both roles; registered => Slave ID 1..247 accepted, 0 and 248 refused (Slave ID is only
     checked when ENABLE = 1 and ROLE = Slave).
     Slave IDs 236 / 237 (the DRF module's own 0xEC / 0xED commands) are refused by the PORT's
     `validate` hook: the Zigbee-IO and DTU ports do, so that is checked by default; a port with
     no hook accepts them (--reserved-ids accept), or skip the check (--reserved-ids skip).
  9. MB_READ / MB_WRITE on the Zigbee pipe (raw port field 4, "port 5" in the Studio):
       accepted while the block is enabled and Zigbee is off (it reports err_code 1 at run time);
       raw 5..15 refused with error 2; an enabled block naming Zigbee while Zigbee is an enabled
       Slave (running OR drafted in the same COMMIT) refused with status 4 / error 7; a DISABLED
       block that only carries the port number is not a conflict.
 10. CLEAR_RULES keeps the Zigbee block; (--reboot) the block and KEY_SET survive a REBOOT;
     (--factory-reset) FACTORY_RESET wipes the block and the key.
 11. Cleanup: CLEAR_RULES, the configuration you started with is written back.

Needs: firmware with SPLC_ZIGBEE_ENABLE=1 (the block is read at 0x1700; otherwise the test stops).
MB_READ checks need SPLC_MBM_ENABLE=1, MB_WRITE checks SPLC_MBW_ENABLE=1 (skipped otherwise).
Checks 8 and 9 that need a Slave role need the port registered with the Slave role
(capability 0x4000 of 0x0A08). Whether the port also supports the Master role cannot be read from
the capabilities: say it with --port-roles, otherwise the Master result is only printed.

WARNING: writes Flash (Zigbee block, MB blocks, rule table) and ends with CLEAR_RULES: rules and MB
blocks already on the board are lost. The network key cannot be read back, so it cannot be restored:
if the board already has a key (KEY_SET = 1) the test refuses to run unless you pass
--allow-key-loss; if it has none, the test leaves a key behind unless you pass --factory-reset
(which wipes EVERYTHING FACTORY_RESET wipes, not only Zigbee). Dev board only.

Usage:
    python test_zigbee_cfg.py COM14
    python test_zigbee_cfg.py COM14 --port-roles both --reboot
    python test_zigbee_cfg.py COM14 --factory-reset
    python test_zigbee_cfg.py COM14 --reserved-ids skip     # a port with no validate hook

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""
import argparse
import sys

import test_report as R   # Excel evidence report (--report / --no-report)
import test_mqtt as T
import test_mpm as M
import test_mbw as W
import test_port_role as P
from test_mqtt import check, info, LinkError

Z_BASE = 0x1700
Z_REGS = 64
Z_CFG = 32
CFG_ERROR = 4                    # CONFIG_STATUS_ERROR
ERR_INVALID = 2                  # CONFIG_ERROR_CODE INVALID_PARAMETER
ERR_SLAVE_PORT_CONFLICT = 7
ZB_PORT_RAW = 4                  # raw port field of an MB block that names the Zigbee pipe (assumption)

CTRL_ENABLE, CTRL_SLAVE, CTRL_ASCII = 0x0001, 0x0002, 0x0004
CTRL_SECURITY, CTRL_ANT, CTRL_PRESERVE = 0x0008, 0x0010, 0x0020
CAP_ZIGBEE, CAP_SLAVE = 0x8000, 0x4000
KEY_A = [0x0102, 0x0304, 0x0506, 0x0708, 0x090A, 0x0B0C, 0x0D0E, 0x0F10]
KEY_B = [0xF1E2, 0xD3C4, 0xB5A6, 0x9788, 0x796A, 0x5B4C, 0x3D2E, 0x1F00]


# ------------------------------------------------------------------ helpers
def image(ctrl=0, sid=0, pan=0, ch=0, r4=0, key=None, rest=None):
    """The 32 registers a host writes (+0..+31). `rest` = 19 values for +13..+31."""
    img = [ctrl, sid, pan, ch, r4] + list(key if key else [0] * 8) + list(rest if rest else [0] * 19)
    assert len(img) == Z_CFG
    return img


def zread(d):
    return d.read(Z_BASE, Z_REGS)


def zput(d, regs, offset=0):
    return d.write_regs(Z_BASE + offset, regs)


def visible(regs):
    """What a read shows of the config part (the key part always reads 0)."""
    return regs[0:5] + regs[13:Z_CFG]


def commit_zb(d, rule, regs, what, expect=None):
    """Draft `regs` at +0, COMMIT. expect None -> returns (s, e); 'ok' / (4, err) -> checks."""
    check(zput(d, regs) is None, f"{what}: draft write ACKed (content is checked at COMMIT)")
    s, e = T.commit(d, [rule])
    if expect == "ok":
        check(s == T.CFG_READY and e == 0, f"{what}: COMMIT accepted (status {s}, error {e})")
    elif expect is not None:
        check(s == CFG_ERROR and e == expect, f"{what}: COMMIT refused with {CFG_ERROR} / {expect} (got {s} / {e})")
    return s, e


def must_refuse(d, rule, regs, what, err=ERR_INVALID):
    """A refused COMMIT: all-or-nothing, version and running block unchanged."""
    before, v0 = zread(d), P.version(d)
    commit_zb(d, rule, regs, what, expect=err)
    check(P.version(d) == v0, f"{what}: the active rule version did not move")
    after = zread(d)
    check(after[0:Z_CFG] == before[0:Z_CFG] and after[36] == before[36],
          f"{what}: the running Zigbee block is unchanged (all-or-nothing)")


def exc(resp):
    return T.exc_code(resp) if resp is not None else None


def reserved_id_checks(d, rule, mode, master_ok):
    """Slave IDs 236 / 237: refused by the port's validate hook (4 / 2), only when the block is
    ENABLED as Slave. mode: 'refuse' (port has the hook), 'accept' (it has none), 'skip'."""
    print("   Slave IDs 236 / 237 (reserved by the DRF module, refused by the port hook)")
    if mode == "skip":
        info("--reserved-ids skip: not checked")
        return
    base = dict(pan=0x1234, ch=15)
    en_slave = CTRL_ENABLE | CTRL_SLAVE
    if mode == "accept":
        for sid in (236, 237):
            commit_zb(d, rule, image(ctrl=en_slave, sid=sid, **base),
                      f"Slave id {sid} (port without a validate hook)", expect="ok")
        commit_zb(d, rule, image(ctrl=0, sid=5, **base), "disable again", expect="ok")
        return

    # Boundaries first: the neighbours are fine, so a refusal below is about 236 / 237 only.
    for sid in (235, 238):
        commit_zb(d, rule, image(ctrl=en_slave, sid=sid, **base), f"Slave id {sid} (next to the reserved ones)",
                  expect="ok")
    for sid in (236, 237):
        must_refuse(d, rule, image(ctrl=en_slave, sid=sid, **base), f"Slave id {sid} (reserved by the module)")

    # All-or-nothing, and the refused draft must not survive into the next COMMIT.
    commit_zb(d, rule, image(ctrl=en_slave, sid=5, **base), "reference: Slave id 5", expect="ok")
    must_refuse(d, rule, image(ctrl=en_slave, sid=236, pan=0x4321, ch=20),
                "Slave id 236 together with a valid PAN / channel change")
    P.expect_ready(d, rule, "plain COMMIT right after the refused 236 (carries no draft)")
    z = zread(d)
    check(z[1] == 5 and z[2] == 0x1234 and z[3] == 15,
          f"the refused draft is gone (id {z[1]}, PAN 0x{z[2]:04X}, ch {z[3]}; expect 5 / 0x1234 / 15)")

    # The hook only looks at an ENABLED Slave.
    commit_zb(d, rule, image(ctrl=CTRL_SLAVE, sid=236, **base),
              "disabled with ROLE = Slave and id 236 (hook only checks an enabled Slave)", expect="ok")
    check(zread(d)[1] == 236, "the disabled block keeps id 236 verbatim")
    if master_ok:
        commit_zb(d, rule, image(ctrl=CTRL_ENABLE, sid=236, **base),
                  "enabled as Master with id 236 (Slave ID is not used by a Master)", expect="ok")
    commit_zb(d, rule, image(ctrl=0, sid=5, **base), "disable again", expect="ok")


# ---------------------------------------------------------------------- run
def run(d, args):
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
    orig, perr = d.try_read(Z_BASE, Z_REGS)
    if perr is not None:
        print(f"  0x1700 is not readable ({perr}): build with SPLC_ZIGBEE_ENABLE 1. Stopping.")
        return
    caps, cerr = d.try_read(M.REG_CAPS, 2)
    capv = ((caps[0] << 16) | caps[1]) if cerr is None else 0
    registered = bool(capv & CAP_ZIGBEE)
    slave_ok = bool(capv & CAP_SLAVE)
    info(f"0x0A08 = 0x{capv:08X}: Zigbee port {'registered' if registered else 'NOT registered'}"
         f"{', Slave role' if slave_ok else ''}")
    key_set = bool(orig[36] & 1)
    print(f"  saved block: control 0x{orig[0]:04X} id {orig[1]} pan 0x{orig[2]:04X} ch {orig[3]} KEY_SET {int(key_set)}")
    if key_set and not args.allow_key_loss:
        print("  The board already holds a network key; this test replaces it and cannot restore it.")
        print("  Run again with --allow-key-loss if that is fine. Stopping.")
        return
    has_mbm = M.try_read_mbm(d)[1] is None
    has_mbw = d.try_read(W.REG_MBW, 1)[1] is None
    info("MB_READ (0x1000) is built" if has_mbm else "MB_READ not built: its checks are skipped")
    info("MB_WRITE (0x1100) is built" if has_mbw else "MB_WRITE not built: its checks are skipped")
    P.expect_ready(d, rule, "baseline COMMIT (drops any stale draft)")

    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")

    try:
        # ------------------------------------------------------------ 1
        print("\n1. Layout and read-back rules")
        z = zread(d)
        check(len(z) == Z_REGS, "64 registers readable at 0x1700")
        check(all(v == 0 for v in z[37:64]), "+37..+63 read 0")
        check(all(v == 0 for v in z[5:13]), "NET_KEY (+5..+12) reads 0")
        check(z[0] & 0xFFC0 == 0 and z[0] & CTRL_PRESERVE == 0, "CONTROL_FLAGS bit 5 and bits 6..15 read 0")
        check(z[36] & 0xFFFE == 0, "STATUS_FLAGS: only bit 0 (KEY_SET) may be set")
        check(d.read(Z_BASE, 32) == z[0:32] and d.read(Z_BASE + 32, 32) == z[32:64],
              "two half reads agree with the full read")

        # ------------------------------------------------------------ 2
        print("\n2. Read-only part and address limits")
        before = zread(d)
        check(zput(d, [0xFFFF] * 4, 32) is None, "write to +32..+35 ACKed (ignored)")
        after = zread(d)
        check(after[36:64] == before[36:64], "STATUS_FLAGS and +37..+63 did not change")
        check(after[0:Z_CFG] == before[0:Z_CFG], "the config part did not change (no draft was opened)")
        resp = d.fc16(Z_BASE + 63, [0, 0])
        check(resp.isError() and exc(resp) == 2, f"FC16 at 0x173F x2 refused with exception 0x02 (got {exc(resp)})")
        resp = d.fc16(Z_BASE - 1, [0, 0])
        check(resp.isError(), "FC16 starting at 0x16FF (below the block) refused")
        P.expect_ready(d, rule, "COMMIT after those writes (must not carry a draft)")
        check(zread(d)[0:Z_CFG] == before[0:Z_CFG], "block unchanged after that COMMIT")

        # ------------------------------------------------------------ 3-4
        print("\n3. Disabled configuration is stored and read back verbatim")
        rest = [0x2222] + [0] * 17 + [0x3333]
        img = image(ctrl=CTRL_ANT, sid=9, pan=0x1234, ch=15, r4=0x1111, rest=rest)
        commit_zb(d, rule, img, "disabled config", expect="ok")
        z = zread(d)
        check(visible(z) == visible(img), "+0..+4 and +13..+31 read back as written")
        check(all(v == 0 for v in z[5:13]), "NET_KEY still reads 0")
        base = zread(d)
        s, e = T.commit(d, [rule])
        check(s == T.CFG_READY and zread(d)[0:Z_CFG] == base[0:Z_CFG],
              "a COMMIT that does not write the block keeps the running configuration")

        # ------------------------------------------------------------ 5
        print("\n4. Refusals (error 2), all-or-nothing, draft dropped")
        run_img = image(ctrl=0, sid=9, pan=0x1234, ch=15, r4=0x1111, rest=rest)
        commit_zb(d, rule, run_img, "reference config", expect="ok")
        must_refuse(d, rule, image(ctrl=CTRL_ASCII, sid=9, pan=0x1234, ch=15), "FRAMING = 1 (ASCII)")
        must_refuse(d, rule, image(ctrl=0x0040, sid=9, pan=0x1234, ch=15), "CONTROL_FLAGS bit 6")
        must_refuse(d, rule, image(ctrl=0x8000, sid=9, pan=0x1234, ch=15), "CONTROL_FLAGS bit 15")
        # a valid change next to a bad flag must not be applied, and must not linger as a draft
        must_refuse(d, rule, image(ctrl=CTRL_ASCII, sid=9, pan=0x4321, ch=15), "valid PAN change + FRAMING = 1")
        P.expect_ready(d, rule, "plain COMMIT right after a refused one")
        check(zread(d)[2] == 0x1234, "the refused draft is gone (PAN is still 0x1234, not 0x4321)")
        commit_zb(d, rule, image(ctrl=CTRL_PRESERVE, sid=9, pan=0x1234, ch=15), "PRESERVE_KEY with SECURITY = 0", expect="ok")
        check(zread(d)[0] & CTRL_PRESERVE == 0, "PRESERVE_KEY is a command: it reads back 0")

        # ------------------------------------------------------------ 6
        print("\n5. A partial write keeps the rest of the block")
        commit_zb(d, rule, image(ctrl=0, sid=9, pan=0x1234, ch=15, r4=0x1111, rest=rest), "reference config", expect="ok")
        check(d.fc06(Z_BASE + 1, 77).isError() is False, "FC06 on +1 (SLAVE_ID) ACKed")
        s, e = T.commit(d, [rule])
        z = zread(d)
        check(s == T.CFG_READY and e == 0, f"COMMIT accepted (status {s}, error {e})")
        check(z[1] == 77 and z[2] == 0x1234 and z[3] == 15 and z[4] == 0x1111 and z[13] == 0x2222 and z[31] == 0x3333,
              "SLAVE_ID changed, PAN / CHANNEL / reserved registers kept")

        # ------------------------------------------------------------ 7
        print("\n6. Network key rules")
        if key_set:
            info("a key was already stored: the 'no key stored' refusals are skipped")
        else:
            must_refuse(d, rule, image(ctrl=CTRL_SECURITY), "SECURITY without a key and without PRESERVE_KEY")
            must_refuse(d, rule, image(ctrl=CTRL_SECURITY, key=[0] * 8), "SECURITY with an all-zero key")
            must_refuse(d, rule, image(ctrl=CTRL_SECURITY | CTRL_PRESERVE), "SECURITY + PRESERVE_KEY with no key stored")
            check(zread(d)[36] & 1 == 0, "KEY_SET is still 0 after those refusals")
        commit_zb(d, rule, image(ctrl=CTRL_SECURITY, sid=9, pan=0x1234, ch=15, key=KEY_A), "SECURITY with a new key", expect="ok")
        z = zread(d)
        check(all(v == 0 for v in z[5:13]), "the stored key reads back 0")
        check(z[36] & 1 == 1, "KEY_SET = 1")
        check(z[0] & CTRL_SECURITY != 0 and z[0] & CTRL_PRESERVE == 0, "SECURITY reads 1, PRESERVE_KEY reads 0")
        check(d.fc06(Z_BASE + 1, 5).isError() is False, "partial write of SLAVE_ID only")
        must_refuse(d, rule, image(ctrl=CTRL_SECURITY, sid=5, pan=0x1234, ch=15),
                    "SECURITY running + draft without PRESERVE_KEY and without a key")
        commit_zb(d, rule, image(ctrl=CTRL_SECURITY | CTRL_PRESERVE, sid=5, pan=0x1234, ch=15), "SECURITY + PRESERVE_KEY", expect="ok")
        check(zread(d)[36] & 1 == 1, "the stored key was kept (KEY_SET = 1)")
        commit_zb(d, rule, image(ctrl=CTRL_SECURITY, sid=5, pan=0x1234, ch=15, key=KEY_B), "replace the key", expect="ok")
        check(zread(d)[36] & 1 == 1 and all(v == 0 for v in zread(d)[5:13]), "KEY_SET = 1, key still reads 0")
        commit_zb(d, rule, image(ctrl=0, sid=5, pan=0x1234, ch=15), "SECURITY = 0", expect="ok")
        check(zread(d)[36] & 1 == 1, "SECURITY = 0 keeps the stored key (KEY_SET = 1) [spec assumption 9]")

        # ------------------------------------------------------------ 8
        print("\n7. Roles the port supports")
        if not registered:
            info("no Zigbee port registered: ENABLE = 1 must be refused for both roles")
            must_refuse(d, rule, image(ctrl=CTRL_ENABLE, sid=0, pan=0x1234, ch=15), "ENABLE as Master, no port")
            must_refuse(d, rule, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=5, pan=0x1234, ch=15), "ENABLE as Slave, no port")
            check(zread(d)[0] & CTRL_ENABLE == 0, "the block is still disabled")
        else:
            master_img = image(ctrl=CTRL_ENABLE, sid=0, pan=0x1234, ch=15)
            s, e = commit_zb(d, rule, master_img, "ENABLE as Master")
            m_ok = (s == T.CFG_READY and e == 0)
            if args.port_roles in ("master", "both"):
                check(m_ok, f"Master enabled: accepted (status {s}, error {e}); --port-roles says the port has it")
            elif args.port_roles == "slave":
                check(not m_ok and (s, e) == (CFG_ERROR, ERR_INVALID),
                      f"Master enabled: refused 4 / 2 (got {s} / {e}); --port-roles says Slave only")
            else:
                info(f"ENABLE as Master -> status {s}, error {e} ({'accepted' if m_ok else 'refused'}); "
                     "say --port-roles to turn this into a check")
            if m_ok:
                check(zread(d)[0] & CTRL_ENABLE != 0, "Master enable reads back")
            commit_zb(d, rule, image(ctrl=0, sid=5, pan=0x1234, ch=15), "disable again", expect="ok")
            if slave_ok:
                for sid in (1, 247, 5):
                    commit_zb(d, rule, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=sid, pan=0x1234, ch=15),
                              f"Slave id {sid}", expect="ok")
                for sid in (0, 248):
                    must_refuse(d, rule, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=sid, pan=0x1234, ch=15), f"Slave id {sid}")
                reserved_id_checks(d, rule, args.reserved_ids, m_ok)
                commit_zb(d, rule, image(ctrl=CTRL_SLAVE, sid=0, pan=0x1234, ch=15),
                          "disabled with ROLE = Slave and id 0 (ID only checked when enabled)", expect="ok")
            else:
                must_refuse(d, rule, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=5, pan=0x1234, ch=15),
                            "ENABLE as Slave when the port has no Slave role")

        # ------------------------------------------------------------ 9
        print("\n8. MB_READ / MB_WRITE on the Zigbee pipe (raw port field 4)")
        commit_zb(d, rule, image(ctrl=0, sid=5, pan=0x1234, ch=15), "Zigbee disabled", expect="ok")
        T.run_syscmd(d, T.SYS_CLEAR_RULES)                         # disables every block, keeps Zigbee
        check(zread(d)[0] & CTRL_ENABLE == 0, "CLEAR_RULES left Zigbee as it was (disabled)")
        if has_mbm:
            vr1 = vr0 + 1
            check(M.write_blocks(d, {0: P.read_block(ZB_PORT_RAW, vr1)}) is None, "MB_READ block on port field 4: draft ACKed")
            P.expect_ready(d, rule, "enabled MB_READ block on Zigbee while Zigbee is off (reports err_code 1 at run time)")
            T.run_syscmd(d, T.SYS_CLEAR_RULES)
            for raw in (5, 9, 15):
                check(M.write_blocks(d, {0: P.read_block(raw, vr1)}) is None, f"MB_READ block on raw port {raw}: draft ACKed")
                P.expect_rejected(d, rule, f"MB_READ on raw port {raw} (no such port)", ERR_INVALID)
            check(M.write_blocks(d, {1: P.disabled_block_with_port(ZB_PORT_RAW)}) is None, "disabled block on port field 4: draft ACKed")
            P.expect_ready(d, rule, "DISABLED block naming Zigbee")
            if slave_ok and registered:
                T.run_syscmd(d, T.SYS_CLEAR_RULES)
                commit_zb(d, rule, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=5, pan=0x1234, ch=15), "Zigbee = enabled Slave", expect="ok")
                check(M.write_blocks(d, {0: P.read_block(ZB_PORT_RAW, vr1)}) is None, "MB_READ block on Zigbee: draft ACKed")
                P.expect_rejected(d, rule, "MB_READ on an enabled Slave Zigbee", ERR_SLAVE_PORT_CONFLICT)
                check(not P.enabled_blocks(M.read_mbm(d), M.BLOCK_REGS), "the refused draft left no MB_READ block running")
                if has_mbw:
                    check(W.write_blocks(d, {0: P.write_block(ZB_PORT_RAW, vr1)}) is None, "MB_WRITE block on Zigbee: draft ACKed")
                    P.expect_rejected(d, rule, "MB_WRITE on an enabled Slave Zigbee", ERR_SLAVE_PORT_CONFLICT)
                    check(not P.enabled_blocks(W.read_mbw(d), W.BLOCK_REGS), "the refused draft left no MB_WRITE block running")
                T.run_syscmd(d, T.SYS_CLEAR_RULES)
                check(M.write_blocks(d, {1: P.disabled_block_with_port(ZB_PORT_RAW)}) is None, "disabled block: draft ACKed")
                P.expect_ready(d, rule, "disabled block naming an enabled Slave Zigbee (not a conflict)")
                print("\n   the same COMMIT makes Zigbee a Slave AND names it: refused, Zigbee stays as it was")
                T.run_syscmd(d, T.SYS_CLEAR_RULES)
                commit_zb(d, rule, image(ctrl=0, sid=5, pan=0x1234, ch=15), "Zigbee disabled", expect="ok")
                before = zread(d)[0:Z_CFG]
                check(zput(d, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=5, pan=0x1234, ch=15)) is None, "draft: Zigbee becomes a Slave")
                check(M.write_blocks(d, {0: P.read_block(ZB_PORT_RAW, vr1)}) is None, "draft: block 0 polls Zigbee")
                P.expect_rejected(d, rule, "same-COMMIT conflict (Zigbee becomes Slave + block names it)", ERR_SLAVE_PORT_CONFLICT)
                check(zread(d)[0:Z_CFG] == before, "Zigbee is still the disabled one (all-or-nothing)")
                print("\n   turning Zigbee into a Slave while a block still polls it is refused as a whole")
                T.run_syscmd(d, T.SYS_CLEAR_RULES)
                check(M.write_blocks(d, {0: P.read_block(ZB_PORT_RAW, vr1)}) is None, "draft: block 0 polls Zigbee (Zigbee off)")
                P.expect_ready(d, rule, "block on Zigbee while Zigbee is off")
                check(zput(d, image(ctrl=CTRL_ENABLE | CTRL_SLAVE, sid=5, pan=0x1234, ch=15)) is None, "draft: Zigbee becomes a Slave")
                P.expect_rejected(d, rule, "Zigbee -> Slave while an enabled block polls it", ERR_SLAVE_PORT_CONFLICT)
            else:
                info("no Zigbee Slave role: the error 7 checks are skipped")
        else:
            info("MB_READ not built: skipped")

        # ------------------------------------------------------------ 10
        print("\n9. CLEAR_RULES keeps Zigbee; REBOOT and FACTORY_RESET")
        T.run_syscmd(d, T.SYS_CLEAR_RULES)
        commit_zb(d, rule, image(ctrl=CTRL_ANT, sid=11, pan=0x2468, ch=20, key=KEY_A), "reference for persistence", expect="ok")
        ref = visible(zread(d))
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
        check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
        z = zread(d)
        check(visible(z) == ref and z[36] & 1 == 1, "CLEAR_RULES did not touch the Zigbee block or the key")
        if args.reboot:
            nd = d.reboot(args)
            if nd is None:
                print("  could not reconnect: stopping")
                return
            d.c, d.kw = nd.c, nd.kw
            z = zread(d)
            check(visible(z) == ref and z[36] & 1 == 1, "after REBOOT the block and KEY_SET are as they were")
        if args.factory_reset:
            s, e = T.run_syscmd(d, T.SYS_FACTORY)
            check(s == T.ST_DONE and e == 0, f"FACTORY_RESET result {s}/{e}")
            z = zread(d)
            check(all(v == 0 for v in z[0:Z_CFG]) and z[36] & 1 == 0, "FACTORY_RESET wiped the block and the key")
        else:
            info("FACTORY_RESET not run (--factory-reset)")
    finally:
        # ------------------------------------------------------------ 11
        print("\n10. Cleanup: no blocks, no rules, the Zigbee configuration you started with")
        try:
            T.run_syscmd(d, T.SYS_CLEAR_RULES)
            back = list(orig[0:Z_CFG])
            for i in range(5, 13):
                back[i] = 0
            back[0] = (back[0] & ~CTRL_PRESERVE) | (CTRL_PRESERVE if back[0] & CTRL_SECURITY else 0)
            commit_zb(d, rule, back, "restore the starting configuration", expect="ok")
            z = zread(d)
            check(visible(z) == visible([orig[0] & ~CTRL_PRESERVE] + orig[1:]),
                  "the Zigbee block reads as it did at the start (the key cannot be compared)")
            if not key_set and z[36] & 1:
                info("a test key is still stored (KEY_SET = 1); run with --factory-reset to wipe it")
            s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
            check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
        except LinkError:
            raise
        except Exception as ex:
            print(f"  (cleanup) FAILED ({ex}): check the Zigbee block on the board")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--port-roles", choices=["auto", "master", "slave", "both"], default="auto",
                   help="roles the registered Zigbee port supports (default auto: Slave from 0x0A08, Master only printed)")
    p.add_argument("--reserved-ids", choices=["refuse", "accept", "skip"], default="refuse",
                   help="Slave IDs 236/237: refuse = the port has the validate hook (Zigbee-IO, DTU; default), "
                        "accept = a port without it, skip = do not check")
    p.add_argument("--allow-key-loss", action="store_true", help="run even if the board already holds a network key")
    p.add_argument("--reboot", action="store_true", help="also check persistence across a REBOOT")
    p.add_argument("--factory-reset", action="store_true", help="also run FACTORY_RESET (wipes everything it wipes)")
    p.add_argument("--port-after", default=None, help="COM port after the REBOOT if the number changes")
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
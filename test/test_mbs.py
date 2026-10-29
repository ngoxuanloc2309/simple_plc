#!/usr/bin/env python3
"""
test_mbs.py - Verify the Modbus SLAVE serving module (plc_mbs, decision 75 in docs/handoff.md,
SPLC-SPEC-SLAVE-001 map) from a Modbus RTU master on the RS-485 port the App set to Slave.

Requires: pip install pyserial           (uses mbs_master.py from the same folder)
Firmware: SPLC_MBM_ENABLE = 1 and SPLC_MODBUS_SLAVE_ENABLE = 1; the App (or a manual write of
the port table 0x1400 through the other link) must have COMMITted the port as Slave:
+0 = enabled | port number, +14 = 1, +15 = Slave ID. The port under test is NOT the App link.

WARNING: the test WRITES tags: DO pins really switch, VFLAG / VREG / VREG_R / COUNTER get test
values. Everything it touched is restored at the end (also after a failure). Dev board only;
disconnect loads from the DO pins first.

What it checks
  1 map     every region reads with the right size; entries/bits the board does not have read 0.
  2 DO/VFLAG  DO_WORD, VFLAG_DWORD and the coils are the same tags (write one, read the other);
            bits beyond the board's DO / VFLAG count are ignored on write.
  3 32-bit  VREG, VREG_R, COUNTER: FC16 writes the whole value (high word first), FC06 changes
            only the addressed half, an entry beyond the board's count reads 0.
  4 errors  FC04 and unknown FC -> 0x01; a hole, a request crossing two regions (DI_WORD,
            DO_WORD and VFLAG_DWORD are THREE regions: 0x0000 x2, 0x0001 x2 (CHKL-SLAVE-04),
            0x0000 x4 all -> 0x02), a coil/DI past its range -> 0x02; quantity 0 / too large,
            bad byte count, bad FC05 value, writes to DI_WORD / AI / SYS_DIAG -> 0x03.
            A write that crosses regions AND touches a read-only one answers 0x02 (App answer
            of 29/10: address/boundary is checked before permission).
  5 sys     SYS_DIAG (ICD v2.12.0): uptime runs; scan_time (u16) plausible; status_flags
            bits 6..15 = 0 and exactly one of ENGINE_RUNNING / FORCE_ACTIVE; reserved words
            0; RTC words are either all 0 (clock not synced / timezone not written) or a
            valid local date and time; writes to SYS_DIAG -> 0x03.
  6 framing garbage in front of a valid frame, another unit's frame in front, a bad CRC (no
            answer, next request still fine), a stale partial frame, 3 requests in ONE burst
            (3 answers), another Slave ID (silence), broadcast write (executes, silent).
  7 speed   N back-to-back reads: all answered, latency figures printed (info only).
What it does NOT check: exception 0x06 while a diag session holds the tags (needs the App link);
the 3.5-character idle rule at the byte level (OS timing is too coarse); a REAL tag in an AI slot.

Usage:
    python test_mbs.py COM35 --unit 5 --baudrate 115200 [--parity N] [--di 4 --do 4 --ai 0
        --vflag 32 --vreg 32 --vret 32 --counter 8] [--speed-n 200] [--trace]
The counts default to the Zigbee-IO board (4 DI, 4 DO, 0 AI, 32 VFLAG, 32 VREG, 32 VREG_R, 8 COUNTER).

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = no link at all.
"""
import argparse
import struct
import sys
import time

import mbs_master as M
from mbs_master import RtuMaster, ModbusException, NoAnswer

_fail = 0
_checks = 0


def check(cond, msg):
    global _fail, _checks
    _checks += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
    if not cond:
        _fail += 1
    return cond


def info(msg):
    print(f"  (info) {msg}")


def section(t):
    print(f"\n== {t}")


def exc_of(fn, *a, **k):
    """Run fn; return the exception code it raised, 0 if it succeeded, -1 on silence."""
    try:
        fn(*a, **k)
        return 0
    except ModbusException as e:
        return e.code
    except NoAnswer:
        return -1


class Ctx:
    pass


# ---------------------------------------------------------------- tests
def t_map(m, c):
    section("1 map")
    di_w = m.read_holding(0x0000, 1)
    do_w = m.read_holding(0x0001, 1)
    vf_w = m.read_holding(0x0002, 2)
    check(len(di_w) == 1 and len(do_w) == 1 and len(vf_w) == 2,
          "DI_WORD (0x0000 x1), DO_WORD (0x0001 x1), VFLAG_DWORD (0x0002 x2) each answer on their own")
    check(di_w[0] >> c.di == 0, f"DI_WORD bits >= {c.di} (no DI there) read 0: 0x{di_w[0]:04X}")
    check(do_w[0] >> c.do == 0, f"DO_WORD bits >= {c.do} (no DO there) read 0: 0x{do_w[0]:04X}")
    if c.vflag < 32:
        check(M.u32_of(vf_w, 0) >> c.vflag == 0, f"VFLAG_DWORD bits >= {c.vflag} read 0")
    d = m.read_discrete(0, 16)
    check(len(d) == 16, "FC02 0..15 answers 16 inputs")
    check(all(v == 0 for v in d[c.di:]), f"DI {c.di}..15 (not on this board) read 0")
    check(sum(b << i for i, b in enumerate(d)) == di_w[0], "FC02 DI bits == DI_WORD (same moment)") \
        or info("DI changed between the two reads? repeat with inputs steady")
    ai = m.read_holding(0x0010, 8)
    check(all(r == 0 for r in ai[2 * c.ai:]), f"AI slots >= {c.ai} read 0")
    big = m.read_holding(0x0020, 64)
    check(len(big) == 64, "VREG region 0x0020..0x005F reads in one request (64 regs)")
    check(all(r == 0 for r in big[2 * c.vreg:]), f"VREG entries >= {c.vreg} read 0")
    check(len(m.read_holding(0x0060, 64)) == 64, "VREG_R region 0x0060..0x009F reads")
    cn = m.read_holding(0x00A0, 16)
    check(all(r == 0 for r in cn[2 * c.counter:]), f"COUNTER entries >= {c.counter} read 0")
    check(len(m.read_holding(0x00F0, 16)) == 16, "SYS_DIAG 0x00F0..0x00FF reads 16 registers")
    check(len(m.read_holding(0x0003, 1)) == 1, "a request may start inside a region (0x0003 x1)")
    check(exc_of(m.read_holding, 0x0000, 4) == 2, "0x0000 x4 spans three regions -> 0x02 (no read across)")


def t_do_vflag(m, c):
    section("2 DO / VFLAG")
    ctx_do, ctx_vf = m.read_holding(0x0001, 1)[0], M.u32_of(m.read_holding(0x0002, 2))
    try:
        # DO_WORD <-> coils
        pattern = 0b1010 & ((1 << c.do) - 1)
        m.write_reg(0x0001, pattern)
        check(m.read_holding(0x0001, 1)[0] == pattern, f"FC06 DO_WORD = 0b{pattern:04b} reads back")
        coils = m.read_coils(0, 16)
        check(sum(b << i for i, b in enumerate(coils)) == pattern, "FC01 coils 0..15 == DO_WORD")
        m.write_coil(0, 1)
        check(m.read_holding(0x0001, 1)[0] == pattern | 1, "FC05 coil 0 ON shows in DO_WORD")
        m.write_coil(0, 0)
        m.write_regs(0x0001, [0xFFFF])
        check(m.read_holding(0x0001, 1)[0] == (1 << c.do) - 1,
              f"DO_WORD = 0xFFFF: only the {c.do} real DO bits stay set (extra bits ignored)")
        m.write_coils(0, [0] * 16)
        check(m.read_holding(0x0001, 1)[0] == 0, "FC15 16 coils OFF clears DO_WORD")
        # VFLAG_DWORD <-> coils 0x64..
        m.write_regs(0x0002, [0x0000, 0x0005])
        check(M.u32_of(m.read_holding(0x0002, 2)) == 0x5, "FC16 VFLAG_DWORD = 0x00000005 reads back")
        vc = m.read_coils(0x0064, 32)
        check(vc[:4] == [1, 0, 1, 0] and sum(vc) == 2, "FC01 0x0064.. shows VFLAG0 and VFLAG2 only")
        m.write_coil(0x0064 + 17, 1)
        check(m.read_holding(0x0002, 1)[0] & 0x0002 == 0x0002,
              "VFLAG17 (coil 0x0075) is bit 1 of the HIGH word (reg 0x0002)")
        m.write_regs(0x0002, [0xFFFF, 0xFFFF])
        want = (1 << c.vflag) - 1 if c.vflag < 32 else 0xFFFFFFFF
        check(M.u32_of(m.read_holding(0x0002, 2)) == want, f"VFLAG_DWORD = all ones -> 0x{want:08X}")
        m.write_coils(0x0064, [0] * 32)
        check(M.u32_of(m.read_holding(0x0002, 2)) == 0, "FC15 32 coils OFF clears VFLAG_DWORD")
    finally:
        m.write_regs(0x0001, [ctx_do])
        m.write_regs(0x0002, M.split_u32(ctx_vf))


def t_32bit(m, c):
    section("3 32-bit entries")
    for name, base, n in (("VREG", 0x0020, c.vreg), ("VREG_R", 0x0060, c.vret), ("COUNTER", 0x00A0, c.counter)):
        if n == 0:
            info(f"{name}: the board has none, skipped")
            continue
        old = m.read_holding(base, 4)
        try:
            m.write_regs(base, [0x1234, 0x5678])
            check(m.read_holding(base, 2) == [0x1234, 0x5678], f"{name}0 FC16 0x12345678 reads back (high word first)")
            m.write_reg(base + 1, 0xABCD)
            check(m.read_holding(base, 2) == [0x1234, 0xABCD], f"{name}0 FC06 on the LOW word changes only it")
            m.write_reg(base, 0x0099)
            check(m.read_holding(base, 2) == [0x0099, 0xABCD], f"{name}0 FC06 on the HIGH word changes only it")
            if n >= 2:
                m.write_regs(base + 2, M.split_u32(-2 & 0xFFFFFFFF))
                got = M.s32(M.u32_of(m.read_holding(base + 2, 2)))
                check(got == -2, f"{name}1 holds -2 (INT32 bits as they are): {got}")
                check(M.u32_of(m.read_holding(base, 2)) == 0x0099ABCD, f"{name}1 write did not touch {name}0")
            # an entry past the board's count: reads 0, a write is ACKed and dropped
            if 2 * n + 2 <= {"VREG": 64, "VREG_R": 64, "COUNTER": 16}[name]:
                ex = base + 2 * n
                check(exc_of(m.write_regs, ex, [0x7777, 0x8888]) == 2,
                      f"{name}{n} (not on this board): FC16 write -> 0x02 (not a silent ACK)")
                check(exc_of(m.write_reg, ex + 1, 1) == 2, f"{name}{n} (not on this board): FC06 write -> 0x02")
                check(m.read_holding(ex, 2) == [0, 0], f"{name}{n} (not on this board): still reads 0, no error")
                if n >= 1:
                    check(exc_of(m.write_regs, base + 2 * n - 2, [1, 2, 3, 4]) == 2,
                          f"{name}: FC16 reaching past the last entry -> 0x02")
                    check(M.u32_of(m.read_holding(base + 2 * n - 2, 2)) != 0x00010002,
                          f"{name}: the refused FC16 changed nothing (all-or-nothing)")
        finally:
            m.write_regs(base, old)


def t_errors(m, c):
    section("4 exceptions")
    check(exc_of(m.read_input, 0, 1) == 1, "FC04 -> 0x01 (only the holding map is served)")
    r = m.transact_raw(M.frame(m.unit, bytes([0x2B, 0x0E, 0x01, 0x00])))
    check(len(r) == 5 and r[1] == 0xAB and r[2] == 1 and M.crc_ok(r), "FC0x2B -> exception 0xAB/0x01 with a good CRC")
    check(exc_of(m.read_holding, 0x0004, 1) == 2, "hole 0x0004 -> 0x02")
    check(exc_of(m.read_holding, 0x0018, 1) == 2, "hole 0x0018 -> 0x02")
    check(exc_of(m.read_holding, 0x00B0, 1) == 2, "hole 0x00B0 -> 0x02")
    check(exc_of(m.read_holding, 0x0100, 1) == 2, "past the map 0x0100 -> 0x02")
    check(exc_of(m.read_holding, 0x0003, 14) == 2, "0x0003..0x0010 crosses two regions -> 0x02")
    check(exc_of(m.read_holding, 0x0000, 2) == 2, "DI_WORD + DO_WORD (0x0000 x2) -> 0x02")
    check(exc_of(m.read_holding, 0x0001, 2) == 2, "DO_WORD + VFLAG_DWORD (0x0001 x2, CHKL-SLAVE-04) -> 0x02")
    check(exc_of(m.read_holding, 0x0000, 4) == 2, "all three status regions in one read (0x0000 x4) -> 0x02")
    check(exc_of(m.read_holding, 0x0002, 3) == 2, "VFLAG_DWORD running into the hole (0x0002 x3) -> 0x02")
    check(exc_of(m.write_regs, 0x0001, [0, 0, 0]) == 2, "FC16 DO_WORD + VFLAG_DWORD (0x0001 x3) -> 0x02")
    check(exc_of(m.write_regs, 0x0002, [0, 0, 0]) == 2, "FC16 VFLAG_DWORD past 0x0003 (0x0002 x3) -> 0x02")
    check(exc_of(m.read_holding, 0x005F, 2) == 2, "VREG / VREG_R boundary crossing -> 0x02")
    check(exc_of(m.read_holding, 0x00F0, 17) == 2, "SYS_DIAG past 0x00FF -> 0x02")
    check(exc_of(m.read_holding, 0x0020, 0) == 3, "FC03 quantity 0 -> 0x03")
    check(exc_of(m.read_holding, 0x0020, 126) == 3, "FC03 quantity 126 -> 0x03")
    check(exc_of(m.write_reg, 0x0000, 1) == 3, "write DI_WORD (RO) -> 0x03")
    check(exc_of(m.write_regs, 0x0000, [1]) == 3, "FC16 on DI_WORD alone (0x0000 x1) -> 0x03")
    check(exc_of(m.write_regs, 0x0000, [1, 2]) == 2,
          "FC16 0x0000 x2 (crosses regions AND touches RO DI_WORD) -> 0x02 (boundary before permission)")
    check(exc_of(m.write_reg, 0x0010, 1) == 3, "write AI (RO) -> 0x03")
    check(exc_of(m.write_reg, 0x00F0, 1) == 3, "write SYS_DIAG (RO) -> 0x03")
    check(exc_of(m.read_coils, 0x0010, 1) == 2, "coil 0x0010 (hole after DO) -> 0x02")
    check(exc_of(m.read_coils, 0x0000, 17) == 2, "coils 0..16 cross out of the DO range -> 0x02")
    check(exc_of(m.read_coils, 0x0064, 33) == 2, "coils 0x0064 x33 pass the VFLAG range -> 0x02")
    check(exc_of(m.read_coils, 0x0000, 0) == 3, "FC01 quantity 0 -> 0x03")
    check(exc_of(m.read_discrete, 0x0010, 1) == 2, "discrete input 0x0010 -> 0x02")
    r = m.transact_raw(M.frame(m.unit, struct.pack(">BHH", 5, 1, 0x1234)))
    check(len(r) >= 3 and r[1] == 0x85 and r[2] == 3, "FC05 value 0x1234 -> 0x03")
    r = m.transact_raw(M.frame(m.unit, struct.pack(">BHHB", 16, 0x0020, 2, 3) + b"\x00\x00\x00"))
    check(len(r) >= 3 and r[1] == 0x90 and r[2] == 3, "FC16 byte count 3 for 2 registers -> 0x03")
    r = m.transact_raw(M.frame(m.unit, struct.pack(">BHHB", 15, 0x0064, 10, 1) + b"\xFF"))
    check(len(r) >= 3 and r[1] == 0x8F and r[2] == 3, "FC15 byte count 1 for 10 coils -> 0x03")
    check(exc_of(m.read_holding, 0x0020, 2) == 0, "the link still works after all of that")


def t_sys(m, c):
    section("5 SYS_DIAG")
    a = m.read_holding(0x00F0, 16)
    time.sleep(2.2)
    b = m.read_holding(0x00F0, 16)
    ua, ub = M.u32_of(a, 0), M.u32_of(b, 0)
    check(1 <= ub - ua <= 4, f"0x00F0..F1 uptime runs: {ua} s -> {ub} s")
    check(b[2] < 1000, f"0x00F2 scan_time_ms (u16) {b[2]} ms is plausible (< 1 s)")
    fl = b[3]
    check(fl >> 6 == 0, f"0x00F3 status_flags bits 6..15 are 0: 0x{fl:04X}")
    running, forced = fl & 0x0001, (fl >> 1) & 1
    check(running != forced,
          f"exactly one of ENGINE_RUNNING ({running}) / FORCE_ACTIVE ({forced}) "
          "(FORCE_ACTIVE = 1 only while the App holds a diagnostic session)")
    check((fl >> 2) & 1 == 0, "BATTERY_LOW = 0 (the board has no battery to measure)")
    check((fl >> 5) & 1 == 0, "RS485_BUS_FAULT = 0 (no detection exists yet)")
    info(f"status_flags 0x{fl:04X}: RTC_SYNCED={(fl >> 3) & 1} FLASH_DIRTY={(fl >> 4) & 1}")
    check(b[4] == 0, "0x00F4 reserved reads 0")
    y, mo, d, h, mi, sec = b[5:11]
    if (y, mo, d, h, mi, sec) == (0, 0, 0, 0, 0, 0):
        info("RTC words 0x00F5..FA are 0: clock not synced, or the Host has not written the "
             "timezone since boot (write 0x0810 first to see a local time)")
    else:
        check(2000 <= y <= 2099 and 1 <= mo <= 12 and 1 <= d <= 31 and h < 24 and mi < 60 and sec < 60,
              f"RTC local time {y:04d}-{mo:02d}-{d:02d} {h:02d}:{mi:02d}:{sec:02d} is a valid date and time")
        check((fl >> 3) & 1 == 1, "a non-zero RTC implies RTC_SYNCED = 1")
        t0 = m.read_holding(0x00F9, 2)
        time.sleep(2.2)
        t1 = m.read_holding(0x00F9, 2)
        dt = ((t1[0] * 60 + t1[1]) - (t0[0] * 60 + t0[1])) % 3600
        check(1 <= dt <= 4, f"RTC seconds advance: +{dt} s in ~2.2 s")
    check(b[11:16] == [0] * 5, "0x00FB..FF reserved read 0")
    check(exc_of(m.write_reg, 0x00F4, 1) == 3, "write 0x00F4 (reserved) -> 0x03")
    check(exc_of(m.write_reg, 0x00F3, 1) == 3, "write status_flags -> 0x03")


def t_framing(m, c):
    section("6 framing")
    good = M.frame(m.unit, struct.pack(">BHH", 3, 0x0002, 2))      # VFLAG_DWORD: one region, 2 regs

    def ok_answer(r):
        return len(r) == 9 and M.crc_ok(r) and r[0] == m.unit and r[1] == 3 and r[2] == 4

    r = m.transact_raw(b"\xFF\x05\x00" + good)
    check(ok_answer(r), "3 garbage bytes in front of a valid frame: answered")
    other = M.frame((m.unit % 247) + 1, struct.pack(">BHH", 3, 0x0002, 2))
    r = m.transact_raw(other + good)
    check(ok_answer(r), "another unit's frame in front, in the same burst: answered")
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    r = m.transact_raw(bytes(bad), timeout=0.3)
    check(r == b"", "bad CRC: no answer")
    check(ok_answer(m.transact_raw(good)), "valid request right after the bad one: answered")
    m.flush()
    m.send_raw(good[:4])
    time.sleep(0.15)                      # the half frame is dead after 3.5 chars of silence
    r = m.transact_raw(good)
    check(ok_answer(r), "stale partial frame (150 ms old) does not spoil the next request")
    m.flush()
    three = good + M.frame(m.unit, struct.pack(">BHH", 3, 0x0001, 1)) + M.frame(m.unit, struct.pack(">BHH", 3, 0x0010, 1))
    m.send_raw(three)
    got = bytearray()
    t_end = time.monotonic() + 1.0
    while time.monotonic() < t_end and len(got) < 9 + 7 + 7:
        got += m.ser.read(64)
    check(len(got) == 23, f"3 requests in one burst -> 3 answers ({len(got)} bytes, expected 23)")
    r = m.transact_raw(M.frame((m.unit % 247) + 1, struct.pack(">BHH", 3, 0x0002, 2)), timeout=0.3)
    check(r == b"", f"request for unit {(m.unit % 247) + 1}: silence")
    # broadcast
    keep = m.read_holding(0x0020, 2)
    try:
        r = m.transact_raw(M.frame(0, struct.pack(">BHH", 6, 0x0021, 0x0B0B)), timeout=0.3)
        check(r == b"", "broadcast FC06: no answer")
        check(m.read_holding(0x0021, 1)[0] == 0x0B0B, "broadcast FC06 was executed")
        r = m.transact_raw(M.frame(0, struct.pack(">BHH", 3, 0x0000, 1)), timeout=0.3)
        check(r == b"", "broadcast read: ignored")
    finally:
        m.write_regs(0x0020, keep)


def t_speed(m, c, n):
    section(f"7 speed ({n} reads)")
    ok, lat = 0, []
    for _ in range(n):
        t0 = time.monotonic()
        try:
            m.read_holding(0x0002, 2)
            ok += 1
            lat.append((time.monotonic() - t0) * 1000)
        except M.ModbusError:
            pass
    check(ok == n, f"{ok}/{n} answered")
    if lat:
        lat.sort()
        info(f"latency ms: min {lat[0]:.1f}  median {lat[len(lat)//2]:.1f}  p95 {lat[int(len(lat)*0.95)-1]:.1f}  max {lat[-1]:.1f}"
             " (one scan = 10 ms; a request is answered in the scan after it arrived)")


def main():
    ap = argparse.ArgumentParser(description="Test the SimplePLC Modbus Slave (plc_mbs)")
    ap.add_argument("port")
    ap.add_argument("--unit", type=int, default=1, help="Slave ID the App set (+15)")
    ap.add_argument("--baudrate", type=int, default=115200)
    ap.add_argument("--parity", choices="NEO", default="N")
    ap.add_argument("--stopbits", type=int, choices=(1, 2), default=1)
    for n, v in (("di", 4), ("do", 4), ("ai", 0), ("vflag", 32), ("vreg", 32), ("vret", 32), ("counter", 8)):
        ap.add_argument(f"--{n}", type=int, default=v, help=f"tags of that kind on the board (default {v})")
    ap.add_argument("--speed-n", type=int, default=200)
    ap.add_argument("--trace", action="store_true")
    a = ap.parse_args()
    c = Ctx()
    c.di, c.do, c.ai, c.vflag, c.vreg, c.vret, c.counter = a.di, a.do, a.ai, a.vflag, a.vreg, a.vret, a.counter
    try:
        m = RtuMaster(a.port, a.baudrate, a.unit, a.parity, a.stopbits, timeout=0.6, trace=a.trace)
    except M.serial.SerialException as e:
        print(f"cannot open {a.port}: {e}")
        return 2
    try:
        try:
            m.read_holding(0x0000, 1)
        except M.ModbusError as e:
            print(f"no link with unit {a.unit} on {a.port}: {e}\n"
                  "Check: port configured as Slave (0x1400 +14 = 1, +15 = ID) and COMMITted, "
                  "firmware built with SPLC_MODBUS_SLAVE_ENABLE=1, A/B wiring, baud/parity.")
            return 3
        for t in (t_map, t_do_vflag, t_32bit, t_errors, t_sys, t_framing):
            try:
                t(m, c)
            except M.ModbusError as e:
                check(False, f"{t.__name__}: unexpected {e}")
        try:
            t_speed(m, c, a.speed_n)
        except M.ModbusError as e:
            check(False, f"t_speed: unexpected {e}")
    finally:
        m.close()
    print(f"\n{_checks} checks, {_fail} failed")
    print("ALL PASS" if _fail == 0 else "FAILED")
    return 0 if _fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""
test_mqtt_tls.py - Verify the MQTT TLS certificate block 0x0F00..0x0F43 of SimplePLC
(decision 51 in docs/handoff.md, plc_mqtt_tls, ICD SPLC-SPEC-MQTT-003) over Modbus.

Requires: pip install pymodbus pyserial
Firmware: SPLC_MQTT_ENABLE=1 and SPLC_MQTT_TLS_CERT_ENABLE=1, AND the host must have called
splc_mqtt_tls_register() (on the Gateway: the host TLS glue). Without it 0x0A08 has no
MQTT_TLS_CERT bit (0x0080) and the block answers exception 0x02: this script then stops
at the first check (the library is fine, the host did not register).

Standalone like test_mbw.py: the Modbus helpers come from test_mqtt.py in the same folder.

WHAT IT DOES TO THE BOARD
  * Without --commit it NEVER stores a certificate: it only sends chunks and checks the
    answers. The host's begin()/chunk() callbacks DO run (the host may buffer the test
    bytes until the next begin(), the transfer is abandoned at the end or after 30 s).
  * With --commit it sends a 250-byte TEST blob and COMMITs it: the host stores it as the
    certificate of --type (default 2, client certificate) and loads it. That REPLACES the
    certificate of that type that is on the device. Use a dev device, or re-send the real
    certificate from Studio afterwards. The blob is not a valid PEM/DER: a host that
    really parses it at commit() may answer STATUS 3 (the script then reports it, it is
    not a library failure).
  * It does not write the rule table or any broker setting, so it does NOT run CLEAR_RULES.

CHECKED (all on the wire, nothing needs a broker):
  * 0x0A08 bit 0x0080; staging and COMMIT read 0; ACK/STATUS/MASK readable.
  * Bad headers (cert_type 0/4, total_chunks 0/33, index >= total, chunk_len 0/121,
    chunk_len > total size, total size < total_chunks or > total_chunks*120): exception 0x03,
    ACK/STATUS unchanged.
  * A chunk of a transfer that was never started: exception 0x03.
  * Chunk CRC: wrong CRC -> ACK stays, STATUS = 1, no exception; CRC computed over the
    padded 120 bytes -> STATUS 1 (the CRC covers chunk_len bytes only); the retry passes.
  * Order: chunk 2 before chunk 1 -> 0x03; chunk 1 resent (lost ACK) -> acknowledged
    again; chunk 1 again with other content -> 0x03; chunk of another type / total_chunks /
    total size, or more bytes than the total size -> 0x03.
  * COMMIT: missing chunk -> 0x03 and the transfer stays open; wrong magic -> 0x03.
  * Register shapes: 63 registers -> 0x03; a write running into ACK, FC06 to a read-only
    register, a 2-register write at COMMIT, a write past 0x0F43 -> 0x02.
  * Chunk 0 in the middle of a transfer restarts it.
  * --wait-abort: a transfer idle for 30 s is abandoned (takes ~32 s).
  * --commit: complete transfer + COMMIT, STATUS 0, MASK bit of the type, a second COMMIT
    with nothing open -> 0x03.

Usage:
    python test_mqtt_tls.py COM14 --unit 1 --baudrate 115200 [--type 2] [--wait-abort] [--commit]

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""

import argparse
import test_report as R   # Excel evidence report (--report / --no-report)
import sys
import time

import test_mqtt as T
from test_mqtt import check, info, LinkError

REG_STAGING = 0x0F00
REG_ACK = 0x0F40
REG_STATUS = 0x0F41
REG_COMMIT = 0x0F42
REG_MASK = 0x0F43
REG_CAPS = 0x0A08
CAP_MQTT_TLS_CERT = 0x0080
MAGIC = 0xA5A5
NO_ACK = 0xFFFF
CHUNK = 120

EXC_ADDR, EXC_VALUE = 2, 3


# ------------------------------------------------------------------ encoding
def encode_chunk(ctype, index, total, size, payload, crc=None, pad_crc=False):
    """64-register image of one SPLC_CertChunk_t (layout of plc_mqtt_tls.h).
    crc: None = the right CRC over the payload bytes; an int = force that value;
    pad_crc = compute the CRC over the payload padded with zeros to 120 bytes (WRONG)."""
    assert 1 <= len(payload) <= CHUNK
    body = bytes(payload).ljust(CHUNK, b"\0")
    if crc is None:
        crc = T.crc16_modbus(body if pad_crc else bytes(payload))
    regs = [(ctype << 8) | index, (total << 8) | len(payload), size, crc]
    regs += [(body[2 * i] << 8) | body[2 * i + 1] for i in range(CHUNK // 2)]
    assert len(regs) == 64
    return regs


def self_check_encoding():
    """The CRC helper must be CRC-16/MODBUS (0x4B37 for '123456789')."""
    assert T.crc16_modbus(b"123456789") == 0x4B37, "crc16_modbus helper is not CRC-16/MODBUS"
    r = encode_chunk(2, 1, 3, 250, b"AB")
    assert r[0] == 0x0201 and r[1] == 0x0302 and r[2] == 250 and r[4] == 0x4142 and r[5] == 0


def blob(n, seed=0):
    return bytes(((i * 7 + seed) & 0x7F) | 0x20 for i in range(n))      # printable, differs per seed


# ------------------------------------------------------------------ device access
def send_regs(d, addr, regs):
    """One FC16, retried only when the frame was lost (a chunk resend is idempotent)."""
    return d.fc16(addr, regs)


def send_chunk(d, regs):
    return send_regs(d, REG_STAGING, regs)


def commit(d, value=MAGIC):
    # not resent behind the script's back: COMMIT is not idempotent
    return d.fc16(REG_COMMIT, [value], safe=False)


def ack_status(d):
    ack = d.read(REG_ACK, 1)[0]
    st = d.read(REG_STATUS, 1)[0]
    return ack, st


def expect_ok(resp, msg):
    ok = not resp.isError()
    return check(ok, f"{msg}: ACK" + ("" if ok else f" (got exception {T.exc_code(resp)})"))


def expect_exc(resp, code, msg):
    got = T.exc_code(resp)
    return check(resp.isError() and got == code,
                 f"{msg}: exception 0x{code:02X}" + ("" if got == code else f" (got {'ACK' if got is None else '0x%02X' % got})"))


# ------------------------------------------------------------------ sections
def section(title):
    print(f"\n== {title}")


def run(d, args):
    ctype = args.type
    cert = blob(250, seed=ctype)            # 250 bytes: chunks of 120 + 120 + 10
    c0, c1, c2 = cert[:120], cert[120:240], cert[240:]

    section("capability and idle registers")
    caps_regs, err = d.try_read(REG_CAPS, 2)
    if err is not None:
        check(False, f"0x0A08 readable ({err}): the build has no capability register (SPLC_MQTT_ENABLE=0?)")
        return
    caps = (caps_regs[0] << 16) | caps_regs[1]
    info(f"0x0A08 = 0x{caps:08X}")
    if not check(bool(caps & CAP_MQTT_TLS_CERT),
                 "0x0A08 reports MQTT_TLS_CERT (0x0080): the host registered its TLS functions"):
        info("without the bit the block is not served: stop here (host glue missing?)")
        return
    check(all(v == 0 for v in d.read(REG_STAGING, 64)), "staging buffer reads 0 (write-only)")
    check(d.read(REG_COMMIT, 1)[0] == 0, "0x0F42 reads 0")
    ack, st = ack_status(d)
    mask0 = d.read(REG_MASK, 1)[0]
    info(f"ACK=0x{ack:04X} STATUS={st} ACTIVE_MASK=0x{mask0:04X} (ACK 0xFFFF = no chunk yet)")
    check(st in (0, 1, 2, 3), "STATUS is one of 0..3")

    section("bad headers: exception 0x03, nothing changes")
    before = ack_status(d)
    pl = blob(10)
    bad = [
        ("cert_type 0", encode_chunk(0, 0, 1, 100, pl)),
        ("cert_type 4", encode_chunk(4, 0, 1, 100, pl)),
        ("total_chunks 0", encode_chunk(ctype, 0, 0, 100, pl)),
        ("total_chunks 33", encode_chunk(ctype, 0, 33, 3000, pl)),
        ("chunk_index == total_chunks", encode_chunk(ctype, 3, 3, 300, pl)),
    ]
    for name, regs in bad:
        expect_exc(send_chunk(d, regs), EXC_VALUE, name)
    r = encode_chunk(ctype, 0, 3, 300, pl); r[1] = (3 << 8) | 0
    expect_exc(send_chunk(d, r), EXC_VALUE, "chunk_len 0")
    r = encode_chunk(ctype, 0, 3, 300, pl); r[1] = (3 << 8) | 121
    expect_exc(send_chunk(d, r), EXC_VALUE, "chunk_len 121")
    expect_exc(send_chunk(d, encode_chunk(ctype, 0, 3, 5, pl)), EXC_VALUE, "chunk_len > total_cert_size")
    expect_exc(send_chunk(d, encode_chunk(ctype, 0, 3, 2, b"A")), EXC_VALUE, "total_cert_size < total_chunks")
    expect_exc(send_chunk(d, encode_chunk(ctype, 0, 3, 361, c0[:60])), EXC_VALUE, "total_cert_size > total_chunks*120")
    after = ack_status(d)
    check(after == before, f"after the bad headers: ACK/STATUS unchanged {before} -> {after}")

    section("a chunk of a transfer that was never started")
    expect_exc(send_chunk(d, encode_chunk(ctype, 5, 6, 700, c0)), EXC_VALUE, "chunk 5 without chunk 0 (idle device)")

    section("CRC")
    ok_chunk0 = encode_chunk(ctype, 0, 3, 250, c0)
    expect_ok(send_chunk(d, encode_chunk(ctype, 0, 3, 250, c0, crc=0x1234)), "chunk 0 with a wrong CRC")
    a, s = ack_status(d)
    check(s == 1 and a == before[0], f"STATUS=1, ACK unchanged (ACK 0x{a:04X}, STATUS {s})")
    expect_ok(send_chunk(d, encode_chunk(ctype, 0, 3, 250, c0[:60], pad_crc=True)), "chunk with the CRC of the 120-byte padded block")
    a, s = ack_status(d)
    check(s == 1, "STATUS=1: the CRC covers chunk_len bytes only")
    expect_ok(send_chunk(d, ok_chunk0), "chunk 0 retried with the right CRC")
    a, s = ack_status(d)
    check(a == 0 and s == 0, f"ACK=0, STATUS=0 after the retry (ACK {a}, STATUS {s})")

    section("order and consistency (transfer of 250 bytes, 3 chunks)")
    expect_exc(send_chunk(d, encode_chunk(ctype, 2, 3, 250, c2)), EXC_VALUE, "chunk 2 before chunk 1 (skipped)")
    check(ack_status(d)[0] == 0, "ACK still 0")
    expect_ok(send_chunk(d, encode_chunk(ctype, 1, 3, 250, c1)), "chunk 1")
    check(ack_status(d)[0] == 1, "ACK=1")
    expect_ok(send_chunk(d, encode_chunk(ctype, 1, 3, 250, c1)), "chunk 1 again (lost ACK): re-acknowledged")
    check(ack_status(d)[0] == 1, "ACK=1 after the retransmission")
    expect_exc(send_chunk(d, encode_chunk(ctype, 1, 3, 250, blob(120, seed=99))), EXC_VALUE, "chunk 1 again with different content")
    other = 1 if ctype != 1 else 3
    expect_exc(send_chunk(d, encode_chunk(other, 2, 3, 250, c2)), EXC_VALUE, "chunk 2 with another cert_type")
    expect_exc(send_chunk(d, encode_chunk(ctype, 2, 4, 250, c2)), EXC_VALUE, "chunk 2 with another total_chunks")
    expect_exc(send_chunk(d, encode_chunk(ctype, 2, 3, 251, c2)), EXC_VALUE, "chunk 2 with another total_cert_size")
    expect_exc(send_chunk(d, encode_chunk(ctype, 2, 3, 250, c1)), EXC_VALUE, "chunk 2 with more bytes than total_cert_size")
    check(ack_status(d)[0] == 1, "ACK still 1 after all the rejections")

    section("COMMIT rules and register shapes")
    expect_exc(commit(d), EXC_VALUE, "COMMIT with a chunk missing")
    check(ack_status(d)[0] == 1, "transfer still open: ACK=1")
    expect_ok(send_chunk(d, encode_chunk(ctype, 2, 3, 250, c2)), "chunk 2 (the transfer stayed open)")
    check(ack_status(d)[0] == 2, "ACK=2")
    for v in (0x0000, 0x5A5A, 0xA5A4):
        expect_exc(commit(d, v), EXC_VALUE, f"COMMIT with 0x{v:04X} on a complete transfer")
    expect_exc(send_regs(d, REG_STAGING, ok_chunk0[:63]), EXC_VALUE, "63 registers: a chunk is exactly 64")
    expect_exc(send_regs(d, REG_STAGING + 1, ok_chunk0), EXC_ADDR, "64 registers from +1: runs into the ACK register")
    for a_ in (REG_ACK, REG_STATUS, REG_MASK):
        expect_exc(d.fc06(a_, 0), EXC_ADDR, f"FC06 to 0x{a_:04X} (read-only)")
    expect_exc(send_regs(d, REG_COMMIT, [MAGIC, 0]), EXC_ADDR, "FC16 of 2 registers from 0x0F42")
    expect_exc(send_regs(d, REG_MASK + 1, [0]), EXC_ADDR, "FC16 past 0x0F43 (unmapped)")
    check(ack_status(d)[0] == 2, "ACK=2: nothing above changed the transfer")

    section("chunk 0 restarts")
    expect_ok(send_chunk(d, ok_chunk0), "chunk 0 again, mid-transfer")
    check(ack_status(d)[0] == 0, "ACK=0: a new transfer")
    expect_exc(send_chunk(d, encode_chunk(ctype, 2, 3, 250, c2)), EXC_VALUE, "chunk 2 right after the restart")

    if args.wait_abort:
        section("30 s watchdog")
        expect_ok(send_chunk(d, ok_chunk0), "chunk 0 (opens a transfer)")
        info("waiting 31 s with an open transfer")
        time.sleep(31.0)
        expect_exc(send_chunk(d, encode_chunk(ctype, 1, 3, 250, c1)), EXC_VALUE, "chunk 1 after 31 s: the transfer was abandoned")
    else:
        info("watchdog not tested (add --wait-abort, takes ~32 s)")

    if args.commit:
        section("COMMIT (stores a certificate in the host)")
        bit = 1 << (ctype - 1)
        if mask0 & bit:
            info(f"the device already has an active certificate of type {ctype}: it will be REPLACED")
        for i, part in enumerate((c0, c1, c2)):
            expect_ok(send_chunk(d, encode_chunk(ctype, i, 3, 250, part)), f"chunk {i}")
            a, s = ack_status(d)
            check(a == i and s == 0, f"chunk {i}: ACK={a} STATUS={s}")
        expect_ok(commit(d), "COMMIT 0xA5A5")
        a, s = ack_status(d)
        mask = d.read(REG_MASK, 1)[0]
        info(f"STATUS={s} ACTIVE_MASK=0x{mask:04X}")
        check(s == 0, "STATUS=0: the host accepted the certificate"
              + ("" if s == 0 else " (2 = overflow, 3 = the host's storage/parser said no; read the host log)"))
        if s == 0:
            check(bool(mask & bit), f"0x0F43 bit {ctype - 1} set")
        expect_exc(commit(d), EXC_VALUE, "a second COMMIT (nothing open)")
    else:
        info("COMMIT not tested (add --commit: it REPLACES the stored certificate of --type)")


# ------------------------------------------------------------------ main
def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--type", type=int, default=2, choices=(1, 2, 3),
                   help="cert_type used by the transfers: 1 Root CA, 2 Client Cert (default), 3 Client Key")
    p.add_argument("--wait-abort", action="store_true", help="also test the 30 s abandon (takes ~32 s)")
    p.add_argument("--commit", action="store_true",
                   help="also COMMIT a 250-byte TEST blob: REPLACES the stored certificate of --type")
    R.add_args(p)
    args = p.parse_args()
    R.start(args)
    self_check_encoding()

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
    except Exception:
        import traceback
        traceback.print_exc()
        print("\n  [ERROR] unexpected exception in the test script (not a firmware verdict)")
        T._failures += 1
    finally:
        try:
            d.c.close()
        except Exception:
            pass
    if T.Dev.retried or T.Dev.retried_w:
        print(f"\n(info) lost frames needing a retry: {T.Dev.retried} read(s), {T.Dev.retried_w} write(s)")
    if link_error is not None:
        print("\nINCONCLUSIVE: a request outcome was unknown (link error), NOT a firmware failure."
              f" {T._failures} check(s) failed before it.")
        sys.exit(3)
    print("\nALL PASS" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
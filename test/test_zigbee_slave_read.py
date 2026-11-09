#!/usr/bin/env python3
"""
test_zigbee_slave_read.py - the PC plays the Zigbee Modbus MASTER (DRF Coordinator on a
USB-UART) and reads the DI / DO of a Zigbee-IO board that serves SLAVE-001 over its own DRF
module (Router, USART2). Everything the master sees is logged to the terminal.

                 PC (this script)                                Zigbee-IO board
  --zb-port  --> DRF Coordinator ~~~~~ radio ~~~~~ DRF Router --> USART2 --> plc_mbs (Slave ID n)
  --app      --> (optional) USB-CDC of the board: used to switch the Zigbee Slave on and to
                 cross-check the DI/DO the radio returned against what the board itself says.

What it does
  0. (needs --app) Probe: capability 0x8000 (Zigbee port registered) and 0x4000 (Slave role),
     read the saved 0x1700 block, remember it.
  1. (needs --app, skipped with --no-config) Write the Slave configuration to the draft
     (ENABLE + ROLE=Slave, Slave ID, PAN/channel as stored values, SECURITY off) and COMMIT
     the rule table that is already running (read back and re-staged, so the rules stay).
  2. Over the radio, RTU frames built here (CRC16, unit = Slave ID):
        FC02 DI0..15   FC01 DO0..15   FC03 0x0000 x2 (DI_WORD, DO_WORD)   FC03 0x00F0 x10 (SYS_DIAG)
     decoded and printed as DI / DO states; DI_WORD/DO_WORD must agree with FC02/FC01.
  3. (needs --app) The same DI/DO read over USB (runtime tags 0x0900..) must equal the radio
     answer (re-read once if a pin moved between the two reads).
  4. Negative cases: address outside the map -> exception 0x02; function code 0x04 -> 0x01;
     a request for another unit ID -> NO answer (the board must stay silent).
  5. A poll loop: --count polls (default 20) every --interval seconds, one log line per poll
     (DI/DO bits, latency, how many chunks the radio delivered the answer in). --monitor
     polls until Ctrl+C.
  6. (--write-test) FC05 DO0 on/off through the radio, read back by FC01 AND by USB.
     This drives a real output: only with the load disconnected.
  7. (needs --app, skipped with --keep) The configuration found at the start is written back.

What it can NOT tell
  - It does not configure the DRF modules. Before running, BOTH modules must already be set up:
    the board's module = Router, transparent transmission, UART 115200 8N1 (the CubeMX
    setting of USART2; the firmware does not touch it), same PAN ID and channel as the
    Coordinator; the PC's module = Coordinator, transparent, UART = --zb-baud.
  - Slave ID 236 / 237 are refused here: the DRF module eats frames starting with 0xEC/0xED.
  - Radio loss is not a firmware verdict: use --retries to ride it out and read the counters
    at the end.

Needs: pip install pyserial pymodbus (pymodbus only for --app).
Firmware: SPLC_ZIGBEE_ENABLE=1, SPLC_MODBUS_LIB=MB, SPLC_MODBUS_SLAVE_ENABLE=1.

WARNING (--app): the COMMIT writes Flash. It re-stages the running rules, but a COMMIT that does
not carry the Timer/Counter blocks (0x0B00..) disables them (decision 8): on a board that uses
function blocks, pass --no-config and configure Zigbee some other way. The network key is not
touched (SECURITY stays off; the stored key, if any, is kept). Dev board only.

Usage:
    python test_zigbee_slave_read.py --zb-port COM20 --app COM14
    python test_zigbee_slave_read.py --zb-port COM20 --app COM14 --unit 3 --count 50 --interval 0.5 -v
    python test_zigbee_slave_read.py --zb-port COM20 --no-config --monitor
    python test_zigbee_slave_read.py --zb-port COM20 --app COM14 --write-test

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open a port, 3 = link error on --app.
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: pip install pyserial")
    sys.exit(2)

Z_BASE = 0x1700
Z_CFG = 32
CAP_ZIGBEE, CAP_SLAVE = 0x8000, 0x4000
CTRL_ENABLE, CTRL_SLAVE, CTRL_SECURITY, CTRL_PRESERVE = 0x0001, 0x0002, 0x0008, 0x0020
REG_CAPS = 0x0A08
# 0x1700 block (SPLC-SPEC-ZIGBEE-006-ICD v2.0.0): offsets used here
O_CTRL, O_CH, O_PAN, O_SID, O_KEY, O_NAME, O_STATUS, O_LINK, O_ERR = 0, 1, 2, 13, 16, 24, 36, 37, 44
# (offset, low, high, default): an ENABLED block is range-checked, so the image written here keeps
# what the board has when that is valid and falls back to these when it is not
RF_FIELDS = ((3, 0, 0xFFFD, 0x0001), (5, 0, 4, 2), (6, 0, 4, 4), (7, 0, 2, 0), (8, 8, 8, 8),
             (9, 1, 2, 1), (10, 50, 5000, 1500), (11, 10, 500, 150), (12, 0, 5, 3))


def zigbee_image(orig, ctrl, unit, pan, chan):
    """32 registers that enable the Slave: the saved block's fields where valid, defaults where
    not, the key part zero (the key is write-only: PRESERVE_KEY keeps the stored one)."""
    img = list(orig[0:Z_CFG])
    img[O_CTRL], img[O_CH], img[O_PAN], img[O_SID] = ctrl, chan, pan, unit
    never_on = not (orig[O_CTRL] & CTRL_ENABLE)      # a block that was never enabled holds zeros, not a setup
    for off, lo, hi, dflt in RF_FIELDS:
        if never_on or not lo <= img[off] <= hi:
            img[off] = dflt
    for i in range(O_KEY, O_NAME):
        img[i] = 0
    if (img[O_NAME + 7] & 0xFF) != 0:                 # the name must end in NUL
        img[O_NAME:Z_CFG] = [0] * 8
    return img

_failures = 0


def check(cond, msg):
    global _failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
    if not cond:
        _failures += 1
    return cond


def info(msg):
    print(f"  [info] {msg}")


# ---------------------------------------------------------------- RTU over the radio
def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build(unit, fc, *words, payload=b""):
    body = bytes([unit, fc])
    for w in words:
        body += bytes([(w >> 8) & 0xFF, w & 0xFF])
    body += payload
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


class Result:
    """One answer (or none) from the radio."""
    def __init__(self):
        self.frame = None        # raw bytes, None = no answer
        self.chunks = []         # (ms since TX, nbytes) arrival log
        self.latency_ms = None   # TX end -> last byte
        self.ok = False          # complete frame, CRC ok, right unit/fc
        self.exc = None          # exception code if the slave answered with one
        self.data = b""          # payload (after byte count for reads)
        self.why = ""            # reason when not ok


class Master:
    """Raw RTU master on the Coordinator's UART. The frame end is cut by LENGTH (never by a
    silence): the radio may deliver one answer in several pieces."""

    def __init__(self, ser, unit, timeout, retries, verbose):
        self.ser, self.unit, self.timeout = ser, unit, timeout
        self.retries, self.verbose = retries, verbose
        self.tx = self.rx_ok = self.timeouts = self.bad = self.retried = 0
        self.lat = []

    @staticmethod
    def expected_len(buf):
        """Total length of the answer once enough bytes are known; None = not yet."""
        if len(buf) < 3:
            return None
        fc = buf[1]
        if fc & 0x80:
            return 5
        if fc in (1, 2, 3, 4):
            return 5 + buf[2]
        if fc in (5, 6, 15, 16):
            return 8
        return -1            # unknown function code: cannot be cut by length

    def _once(self, frame, unit, fc):
        r = Result()
        self.ser.reset_input_buffer()
        t0 = time.perf_counter()
        self.ser.write(frame)
        self.ser.flush()
        self.tx += 1
        t_tx = time.perf_counter()
        if self.verbose:
            print(f"      TX {hexs(frame)}")
        buf = b""
        deadline = t_tx + self.timeout
        while time.perf_counter() < deadline:
            n = self.ser.in_waiting
            if n:
                got = self.ser.read(n)
                r.chunks.append((round((time.perf_counter() - t_tx) * 1000.0), len(got)))
                buf += got
                need = self.expected_len(buf)
                if need == -1:
                    r.why = f"unknown function code 0x{buf[1]:02X} in the answer"
                    break
                if need is not None and len(buf) >= need:
                    break
            else:
                time.sleep(0.001)
        r.latency_ms = (time.perf_counter() - t_tx) * 1000.0
        r.frame = buf if buf else None
        if self.verbose:
            if buf:
                print(f"      RX {hexs(buf)}   chunks(ms,bytes)={r.chunks}")
            else:
                print(f"      RX (nothing in {self.timeout:.1f} s)")
        if not buf:
            r.why = "no answer"
            return r
        need = self.expected_len(buf)
        if need is None or need < 0 or len(buf) < need:
            r.why = f"short frame ({len(buf)} bytes, expected {need})"
            return r
        if len(buf) > need:
            r.why = f"{len(buf) - need} extra byte(s) after the frame"
            return r
        if crc16(buf[:-2]) != (buf[-2] | (buf[-1] << 8)):
            r.why = "CRC error"
            return r
        if buf[0] != unit:
            r.why = f"answer from unit {buf[0]}, asked {unit}"
            return r
        if buf[1] == (fc | 0x80):
            r.exc, r.ok = buf[2], True
            return r
        if buf[1] != fc:
            r.why = f"answer function code 0x{buf[1]:02X}, asked 0x{fc:02X}"
            return r
        r.ok = True
        r.data = buf[3:-2] if fc in (1, 2, 3, 4) else buf[2:-2]
        return r

    def request(self, fc, *words, payload=b"", unit=None, retries=None):
        u = self.unit if unit is None else unit
        frame = build(u, fc, *words, payload=payload)
        n = self.retries if retries is None else retries
        for attempt in range(n + 1):
            r = self._once(frame, u, fc)
            if r.ok:
                self.rx_ok += 1
                self.lat.append(r.latency_ms)
                if attempt:
                    self.retried += attempt
                return r
            if r.frame is None:
                self.timeouts += 1
            else:
                self.bad += 1
            if attempt < n:
                time.sleep(0.2)
        self.retried += n
        return r

    # decoded helpers -------------------------------------------------------------
    def bits(self, fc, addr, qty):
        r = self.request(fc, addr, qty)
        if not r.ok or r.exc is not None or len(r.data) != (qty + 7) // 8:
            return None, r
        v = [(r.data[i // 8] >> (i % 8)) & 1 for i in range(qty)]
        return v, r

    def regs(self, addr, qty):
        r = self.request(3, addr, qty)
        if not r.ok or r.exc is not None or len(r.data) != 2 * qty:
            return None, r
        return [(r.data[2 * i] << 8) | r.data[2 * i + 1] for i in range(qty)], r


def bitstr(v, n):
    """DI0 first, e.g. '1010'."""
    return "".join(str(b) for b in v[:n])


# ---------------------------------------------------------------- the App link (USB)
class App:
    """Thin wrapper over test_mqtt's Dev: only what this script needs."""

    def __init__(self, port, baud, unit):
        import test_mqtt as T
        self.T = T
        self.client = T.make_client(port, baud)
        if not self.client.connect():
            raise OSError(f"cannot open {port}")
        self.d = T.Dev(self.client, unit)

    def close(self):
        try:
            self.client.close()
        except Exception:
            pass

    def caps(self):
        v, err = self.d.try_read(REG_CAPS, 2)
        return ((v[0] << 16) | v[1]) if err is None else 0

    def rules(self):
        """The running rule table as a list of 16-register records."""
        n = self.d.read(self.T.REG_RULE_INFO, 1)[0]
        out = []
        for i in range(n):
            out.append(self.d.read(0x0100 + 16 * i, 16))
        return out

    def commit_keep_rules(self, rules):
        return self.T.commit(self.d, rules)

    def tag_bits(self, di, do):
        """DI and DO values from the runtime tag window (2 registers per tag, high word first;
        dense layout: DI first, then DO)."""
        n = di + do
        regs = self.d.read(self.T.REG_TAGS, 2 * n)
        vals = [((regs[2 * i] << 16) | regs[2 * i + 1]) & 1 for i in range(n)]
        return vals[:di], vals[di:di + do]


def write_zigbee_block(app, regs):
    return app.d.write_regs(Z_BASE, regs)


# ---------------------------------------------------------------- phases
def phase_config(app, args):
    """Phases 0, 1: returns the saved block (or None) and whether the Slave is expected on."""
    d = app.d
    print("\n0. Probe over the App link")
    caps = app.caps()
    info(f"0x0A08 = 0x{caps:08X}")
    if not check(bool(caps & CAP_ZIGBEE), "capability 0x8000 ZIGBEE: the board registered its Zigbee port"):
        print("  -> no Zigbee port: firmware without SPLC_ZIGBEE_ENABLE, or the port did not register. Stopping.")
        return None, False
    if not check(bool(caps & CAP_SLAVE), "capability 0x4000 MODBUS_SLAVE: the Slave role is served"):
        print("  -> the port dropped the Slave role (needs SLAVE_ENABLE=1 and LIB=MB). Stopping.")
        return None, False
    orig = d.read(Z_BASE, 64)
    info(f"saved block: control 0x{orig[O_CTRL]:04X} slave_id {orig[O_SID]} pan 0x{orig[O_PAN]:04X} "
         f"channel {orig[O_CH]} KEY_SET {orig[O_STATUS] & 1} link_state {orig[O_LINK]} error {orig[O_ERR]}")

    if args.no_config:
        run = d.read(Z_BASE, O_SID + 1)
        on = bool(run[0] & CTRL_ENABLE) and bool(run[0] & CTRL_SLAVE)
        check(on, f"--no-config: the board is already an enabled Slave (control 0x{run[0]:04X})")
        if on and run[O_SID] != args.unit:
            info(f"the board's Slave ID is {run[O_SID]}, not --unit {args.unit}: using {run[O_SID]}")
            args.unit = run[O_SID]
        return orig, on

    print("\n1. Switch the Zigbee Slave on (draft + COMMIT)")
    rules = app.rules()
    info(f"{len(rules)} running rule(s) will be re-staged unchanged")
    ctrl = CTRL_ENABLE | CTRL_SLAVE
    if orig[0] & CTRL_SECURITY:
        info("security is ON in the saved block: kept (PRESERVE_KEY), the stored key is not touched")
        ctrl |= CTRL_SECURITY | CTRL_PRESERVE
    pan = args.pan if args.pan is not None else (orig[O_PAN] or 0x1234)
    chan = args.channel if args.channel is not None else (orig[O_CH] or 15)
    img = zigbee_image(orig, ctrl, args.unit, pan, chan)
    check(write_zigbee_block(app, img) is None, "draft write of 0x1700..0x171F ACKed")
    s, e = app.commit_keep_rules(rules)
    if not check(s == app.T.CFG_READY and e == 0, f"COMMIT accepted (status {s}, error {e})"):
        return orig, False
    run = d.read(Z_BASE, 64)
    check(run[0] & 0x1F == (ctrl & 0x1F), f"running CONTROL_FLAGS = 0x{run[0]:04X} (Enable + Slave)")
    check(run[O_SID] == args.unit, f"running SLAVE_ID = {run[O_SID]}")
    info(f"LINK_STATE {run[O_LINK]}  ERROR_CODE {run[O_ERR]} (the port reports 0 unless the Slave ID is 236/237)")
    time.sleep(0.3)      # let plc_mbs open the server on the next scan
    return orig, True


def phase_radio(m, app, res, args):
    di_n, do_n = res if res else (4, 4)
    print(f"\n2. Read DI / DO over the radio (unit {m.unit})")
    di16, r1 = m.bits(2, 0x0000, 16)
    if not check(di16 is not None, f"FC02 DI0..15: {'answer received' if di16 is not None else r1.why or ('exception %s' % r1.exc)}"):
        print("  -> no usable answer. Check: both DRF modules (PAN/channel/transparent/115200), the "
              "Slave ID, --zb-baud, and with -v the raw bytes.")
        return None
    do16, r2 = m.bits(1, 0x0000, 16)
    check(do16 is not None, f"FC01 DO0..15: {'answer received' if do16 is not None else r2.why or ('exception %s' % r2.exc)}")
    if do16 is None:
        return None
    print(f"      DI0..{di_n - 1} = {bitstr(di16, di_n)}   (DI0 first; bits >= {di_n} must read 0)")
    print(f"      DO0..{do_n - 1} = {bitstr(do16, do_n)}   (DO0 first; bits >= {do_n} must read 0)")
    check(not any(di16[di_n:]), f"DI{di_n}..15 read 0 (the board has {di_n} DI)")
    check(not any(do16[do_n:]), f"DO{do_n}..15 read 0 (the board has {do_n} DO)")

    w, r3 = m.regs(0x0000, 1)
    w2, r4 = m.regs(0x0001, 1)
    if check(w is not None and w2 is not None, "FC03 DI_WORD (0x0000) and DO_WORD (0x0001) answered"):
        di_word = sum(b << i for i, b in enumerate(di16))
        do_word = sum(b << i for i, b in enumerate(do16))
        if w[0] != di_word or w2[0] != do_word:       # a pin may have moved: read the bits again
            di16b, _ = m.bits(2, 0, 16)
            do16b, _ = m.bits(1, 0, 16)
            di_word = sum(b << i for i, b in enumerate(di16b or di16))
            do_word = sum(b << i for i, b in enumerate(do16b or do16))
        check(w[0] == di_word, f"DI_WORD 0x{w[0]:04X} == FC02 bits 0x{di_word:04X}")
        check(w2[0] == do_word, f"DO_WORD 0x{w2[0]:04X} == FC01 bits 0x{do_word:04X}")

    diag, r5 = m.regs(0x00F0, 10)
    if check(diag is not None, "FC03 SYS_DIAG 0x00F0 x10 answered"):
        up = (diag[0] << 16) | diag[1]
        fl = diag[3]
        print(f"      uptime {up} s  scan {diag[2]} ms  status_flags 0x{fl:04X} "
              f"[engine_running={fl & 1} force={fl >> 1 & 1} rtc_synced={fl >> 3 & 1} flash_dirty={fl >> 4 & 1}]")
        check(fl & 0xFFC0 == 0, "status_flags bits 6..15 = 0")
        check(0 < diag[2] < 1000 or diag[2] == 0, f"scan_time {diag[2]} ms is plausible")

    if app is not None:
        print("\n3. Cross-check against what the board says over USB")
        for attempt in range(2):
            udi, udo = app.tag_bits(di_n, do_n)
            rdi, _ = m.bits(2, 0, di_n)
            rdo, _ = m.bits(1, 0, do_n)
            if rdi is not None and rdo is not None and rdi == udi and rdo == udo:
                break
            time.sleep(0.1)
        check(rdi == udi, f"DI over radio {bitstr(rdi or [], di_n)} == DI over USB {bitstr(udi, di_n)}")
        check(rdo == udo, f"DO over radio {bitstr(rdo or [], do_n)} == DO over USB {bitstr(udo, do_n)}")
    return di16, do16


def phase_negative(m):
    print("\n4. Negative cases")
    r = m.request(3, 0x0200, 1, retries=1)
    check(r.ok and r.exc == 2, f"FC03 at 0x0200 (outside the map) -> exception 0x02 (got "
                               f"{('exc %s' % r.exc) if r.ok else r.why})")
    r = m.request(3, 0x0000, 2, retries=1)          # crosses DI_WORD / DO_WORD: two regions
    check(r.ok and r.exc == 2, f"FC03 0x0000 x2 crosses two regions -> exception 0x02 (got "
                               f"{('exc %s' % r.exc) if r.ok else r.why})")
    r = m.request(4, 0x0000, 1, retries=1)
    check(r.ok and r.exc == 1, f"FC04 (not served) -> exception 0x01 (got "
                               f"{('exc %s' % r.exc) if r.ok else r.why})")
    other = 7 if m.unit != 7 else 8
    r = m.request(3, 0x0000, 1, unit=other, retries=0)
    check(r.frame is None, f"a request for unit {other} gets NO answer "
                           f"({'silent' if r.frame is None else 'answered: ' + hexs(r.frame)})")


def phase_poll(m, app, res, args):
    di_n, do_n = res if res else (4, 4)
    print(f"\n5. Poll loop ({'until Ctrl+C' if args.monitor else str(args.count) + ' polls'}, "
          f"every {args.interval:.2f} s)")
    print("      #   time      DI" + " " * max(1, di_n - 1) + "DO     ms  chunks  note")
    i, prev = 0, None
    try:
        while args.monitor or i < args.count:
            i += 1
            t0 = time.time()
            di, r1 = m.bits(2, 0, 16)
            do, r2 = m.bits(1, 0, 16)
            stamp = time.strftime("%H:%M:%S")
            if di is None or do is None:
                why = (r1.why if di is None else r2.why) or "exception"
                print(f"   {i:4d}  {stamp}  --    --    --   --    {why}")
                check(False, f"poll {i}: no valid answer ({why})")
            else:
                cur = (bitstr(di, di_n), bitstr(do, do_n))
                note = "" if prev in (None, cur) else "changed"
                prev = cur
                lat = (r1.latency_ms + r2.latency_ms) / 2.0
                print(f"   {i:4d}  {stamp}  {cur[0]}  {cur[1]}  {lat:5.0f}  "
                      f"{len(r1.chunks)}+{len(r2.chunks)}   {note}")
            dt = args.interval - (time.time() - t0)
            if dt > 0:
                time.sleep(dt)
    except KeyboardInterrupt:
        print("\n   (stopped)")


def phase_write(m, app, res):
    di_n, do_n = res if res else (4, 4)
    print("\n6. Write test: FC05 DO0 through the radio (drives a real output)")
    base, _ = m.bits(1, 0, 1)
    if base is None:
        check(False, "cannot read DO0 first")
        return
    for want in (1 - base[0], base[0]):
        r = m.request(5, 0x0000, 0xFF00 if want else 0x0000, retries=1)
        if r.ok and r.exc == 6:
            info("exception 0x06: a diagnostic session holds the tags (writes are refused then)")
            check(False, "FC05 refused with 0x06")
            return
        check(r.ok and r.exc is None, f"FC05 DO0 = {want} acknowledged (echo)")
        time.sleep(0.15)
        back, _ = m.bits(1, 0, 1)
        check(back is not None and back[0] == want, f"FC01 reads DO0 = {want}")
        if app is not None:
            _, udo = app.tag_bits(di_n, do_n)
            check(udo[0] == want, f"USB runtime tag DO0 = {want}")
    info("DO0 is back to its original state (unless a rule drives it)")


def phase_restore(app, orig):
    print("\n7. Restore the configuration found at the start")
    ctrl = orig[0] & 0x1F
    if orig[0] & CTRL_SECURITY:
        ctrl |= CTRL_PRESERVE
    img = list(orig[0:Z_CFG])               # exactly what was saved: a block that was valid stays valid
    img[O_CTRL] = ctrl
    for i in range(O_KEY, O_NAME):
        img[i] = 0                          # the key reads 0 and is kept by PRESERVE_KEY
    rules = app.rules()
    write_zigbee_block(app, img)
    s, e = app.commit_keep_rules(rules)
    check(s == app.T.CFG_READY and e == 0, f"COMMIT of the original block (status {s}, error {e})")
    now = app.d.read(Z_BASE, O_SID + 1)
    check(now[O_CTRL] == orig[O_CTRL] & 0x1F and now[O_SID] == orig[O_SID], "the original block reads back")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--zb-port", required=True, help="serial port of the PC's DRF Coordinator")
    p.add_argument("--zb-baud", type=int, default=115200, help="UART speed of the PC's module (default 115200)")
    p.add_argument("--app", default=None, help="USB-CDC port of the board (config + cross-check)")
    p.add_argument("--app-baud", type=int, default=115200)
    p.add_argument("--app-unit", type=int, default=1, help="Modbus unit of the App link (default 1)")
    p.add_argument("--unit", type=int, default=1, help="Zigbee Slave ID (1..247, not 236/237; default 1)")
    p.add_argument("--pan", type=lambda x: int(x, 0), default=None, help="PAN ID stored in 0x1700 (information only)")
    p.add_argument("--channel", type=int, default=None, help="channel stored in 0x1700 (information only)")
    p.add_argument("--no-config", action="store_true", help="do not touch 0x1700: the Slave is already on")
    p.add_argument("--keep", action="store_true", help="leave the Slave configuration on the board at the end")
    p.add_argument("--count", type=int, default=20, help="polls in the loop (default 20)")
    p.add_argument("--interval", type=float, default=1.0, help="seconds between polls (default 1.0)")
    p.add_argument("--monitor", action="store_true", help="poll until Ctrl+C")
    p.add_argument("--timeout", type=float, default=2.0, help="answer timeout in seconds (default 2.0; radio is slow)")
    p.add_argument("--retries", type=int, default=2, help="resends of a lost request (default 2)")
    p.add_argument("--di", type=int, default=4, help="DI count of the board (default 4)")
    p.add_argument("--do", type=int, default=4, help="DO count of the board (default 4)")
    p.add_argument("--write-test", action="store_true", help="also toggle DO0 with FC05 (real output!)")
    p.add_argument("-v", "--verbose", action="store_true", help="print every TX/RX frame in hex with chunk timing")
    args = p.parse_args()

    if not 1 <= args.unit <= 247 or args.unit in (236, 237):
        p.error("--unit must be 1..247 and not 236/237 (the DRF module treats 0xEC/0xED as commands)")
    if args.app is None:
        info("no --app: no configuration, no USB cross-check; the Slave must already be on")

    try:
        ser = serial.Serial(args.zb_port, args.zb_baud, bytesize=8, parity="N", stopbits=1, timeout=0)
    except serial.SerialException as e:
        print(f"Cannot open {args.zb_port}: {e}")
        sys.exit(2)

    app = None
    orig = None
    link_error = None
    m = None
    try:
        if args.app:
            try:
                app = App(args.app, args.app_baud, args.app_unit)
            except Exception as e:
                print(f"Cannot open the App link {args.app}: {e}")
                sys.exit(2)
            import test_mqtt as T
            try:
                orig, on = phase_config(app, args)
            except T.LinkError as e:
                link_error = e
                orig, on = None, False
            if orig is None or not on:
                if link_error is None:
                    print("\nStopping: the Slave is not on.")
                raise SystemExit(3 if link_error else 1)
        m = Master(ser, args.unit, args.timeout, args.retries, args.verbose)
        res = (args.di, args.do)
        if app is not None:
            r = app.d.read(app.T.REG_RESOURCE, 10)
            res = (r[3], r[4])
            info(f"board resource: DI={res[0]} DO={res[1]}")
        out = phase_radio(m, app, res, args)
        if out is not None:
            phase_negative(m)
            phase_poll(m, app, res, args)
            if args.write_test:
                phase_write(m, app, res)
    except SystemExit:
        raise
    except Exception as e:                          # a LinkError from the USB side, or a bug here
        import traceback
        if app is not None and type(e).__name__ == "LinkError":
            link_error = e
            print(f"\n  [LINK] {e}")
        else:
            traceback.print_exc()
            print("\n  [ERROR] unexpected exception in the test script (not a firmware verdict)")
            global _failures
            _failures += 1
    finally:
        if app is not None and orig is not None and not args.keep and not args.no_config:
            try:
                phase_restore(app, orig)
            except Exception as e:
                print(f"  [WARN] could not restore the original Zigbee block: {e}")
        if app is not None:
            app.close()
        ser.close()

    if m is not None and m.tx:
        avg = sum(m.lat) / len(m.lat) if m.lat else 0.0
        print(f"\nRadio: {m.tx} frames sent, {m.rx_ok} answered, {m.timeouts} timeouts, "
              f"{m.bad} bad frames, {m.retried} resends; latency "
              f"avg {avg:.0f} ms, max {max(m.lat) if m.lat else 0:.0f} ms")
    if link_error is not None:
        sys.exit(3)
    print("\nALL PASS" if _failures == 0 else f"\n{_failures} CHECK(S) FAILED")
    sys.exit(0 if _failures == 0 else 1)


if __name__ == "__main__":
    main()
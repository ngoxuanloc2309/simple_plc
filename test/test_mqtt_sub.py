#!/usr/bin/env python3
"""
test_mqtt_sub.py - Verify the MQTT Subscribe blocks 0x0D80..0x0DFF of SimplePLC (decision 47
in docs/handoff.md, plc_mqtt_sub, ICD SPLC-SPEC-MQTT-002) over Modbus RTU.

Requires: pip install pymodbus pyserial        (plus paho-mqtt only for --live)
Firmware must be built with SPLC_MQTT_ENABLE=1 and SPLC_MQTT_SUB_ENABLE=1.

Standalone like test_mbw.py, whose helpers (test_mqtt.py, test_tagtype.py) it imports from
the same folder.

WARNING: this test WRITES Flash (rule table + SUB blocks), REPLACES the rule table and runs
CLEAR_RULES / FACTORY_RESET. Run it on a dev board only. It ALSO wipes the MQTT Publish and
MB_READ configuration that is on the board: Deploy again from Studio afterwards.

PART 1 - CONFIGURATION PATH (always runs, no broker needed), layout of plc_mqtt_sub.h:
  * FC16/FC06 into 0x0D80..0x0DFF go to a DRAFT; reads return the RUNNING blocks.
  * COMMIT validates every drafted block first. A bad ENABLED block (QoS 3, fail-safe action 3,
    empty or malformed topic filter, pin count outside 1..4, a target tag that does not exist,
    data type outside 1..3, a non-printable key) rejects the WHOLE COMMIT
    (CONFIG_STATUS != READY, error 2); the running blocks do not change.
  * A block with the enable bit clear is not checked.
  * The target tag KIND is NOT restricted (decision 47): DI/DO/AI/COUNTER targets are accepted
    when the board has them; a pin with target 0xFFFF is skipped.
  * A Deploy that does not write the blocks keeps them running.
  * FC06 changes one register; the rest of the block is kept.
  * Only a request that runs past 0x0DFF answers exception 0x02.
  * Capability bit MQTT_SUB (0x20) of 0x0A08 is set.
  * The blocks survive REBOOT; CLEAR_RULES and FACTORY_RESET clear them, CLEAR_RETAIN does not.

PART 2 - RECEIVING MESSAGES (only with --live HOST[:PORT], needs paho-mqtt):
  The script publishes to a broker and reads the target tags over Modbus. REQUIREMENTS: the
  board must be CONNECTED to that same broker (the host default broker, or one set from Studio)
  and reachable from this PC. Checked: JSON path keys, atomic apply (a message with a missing
  key writes nothing), a raw-payload block with rounding to INT32. This part also needs the
  host glue (mqtt_client_set_subscriptions) that has NOT run on a board before: a failure
  here can be the glue or the network, not only plc_mqtt_sub. Read the log on the board.

Usage:
    python test_mqtt_sub.py COM17 --unit 1 --baudrate 115200 [--reboot] [--port-after COM18]
    python test_mqtt_sub.py COM17 --live 192.168.1.10:1883 [--live-topic splc/test/sub]

Exit codes: 0 = ALL PASS, 1 = a check failed, 2 = cannot open port, 3 = link error.
"""
import argparse
import test_report as R   # Excel evidence report (--report / --no-report)
import sys
import time

import test_mqtt as T
import test_tagtype as TT
from test_mqtt import check, info, LinkError

REG_SUB = 0x0D80
BLOCKS, BLOCK_REGS = 2, 64
SUB_REGS = BLOCKS * BLOCK_REGS
NONE = 0xFFFF
DT_BOOL, DT_INT32, DT_REAL = 1, 2, 3
FS_KEEP, FS_ZERO, FS_DEFAULT = 0, 1, 2
REG_CAPS = 0x0A08
CAP_MQTT_SUB = 0x20
READ_CHUNK = 64


# ------------------------------------------------------------------ encoding
def str_regs(s, nregs):
    """Two characters per register, first character in the HIGH byte, NUL padded."""
    b = s.encode("latin-1")[:nregs * 2].ljust(nregs * 2, b"\0")
    return [(b[2 * i] << 8) | b[2 * i + 1] for i in range(nregs)]


def pin(tag=NONE, dt=DT_INT32, default=0, key=""):
    return [tag, dt, (default >> 16) & 0xFFFF, default & 0xFFFF, *str_regs(key, 6)]


def block(enabled=True, qos=0, fifo=False, ign_retain=False, fs=FS_KEEP, timeout=0,
          topic="splc/test/a", pins=(), count=None):
    """64-register image of one SUB block (layout of plc_mqtt_sub.h)."""
    r = [0] * BLOCK_REGS
    r[0] = ((1 if enabled else 0) | (qos << 1) | ((1 if fifo else 0) << 3)
            | ((1 if ign_retain else 0) << 4) | (fs << 5))
    r[2], r[3] = (timeout >> 16) & 0xFFFF, timeout & 0xFFFF
    r[4] = len(pins) if count is None else count
    r[8:24] = str_regs(topic, 16)
    for i, p in enumerate(pins[:4]):
        r[24 + 10 * i:24 + 10 * (i + 1)] = p
    return r


def read_sub(d):
    out = []
    for off in range(0, SUB_REGS, READ_CHUNK):
        out += d.read(REG_SUB + off, READ_CHUNK)
    return out


def image(blocks):
    img = [0] * SUB_REGS
    for i, b in blocks.items():
        img[i * BLOCK_REGS:(i + 1) * BLOCK_REGS] = b
    return img


def write_blocks(d, blocks):
    for i, b in blocks.items():
        resp = d.write_regs(REG_SUB + i * BLOCK_REGS, b)
        if resp is not None:
            return resp
    return None


def deploy(d, rule, blocks):
    if blocks:
        resp = write_blocks(d, blocks)
        if resp is not None:
            return None, None, resp
    s, e = T.commit(d, [rule])
    return s, e, None


def deploy_ok(d, rule, blocks, what):
    s, e, resp = deploy(d, rule, blocks)
    return check(resp is None and s == T.CFG_READY and e == 0,
                 f"{what}: COMMIT ok (status {s}, error {e})")


def deploy_rejected(d, rule, blocks, what):
    s, e, resp = deploy(d, rule, blocks)
    check(resp is None, f"{what}: draft write ACKed (content is checked at COMMIT)")
    return check(s != T.CFG_READY and e == T.ERR_INVALID_PARAMETER,
                 f"{what}: COMMIT rejected (status {s}, error {e}; expect error 2)")


def self_check_encoding():
    """Offline sanity of the encoder against the documented layout (runs before touching the board)."""
    b = block(qos=1, fs=FS_DEFAULT, timeout=0x12345, topic="ab/c",
              pins=[pin(7, DT_REAL, 0x3F800000, "x.y")])
    assert len(b) == BLOCK_REGS
    assert b[0] == (1 | (1 << 1) | (2 << 5))
    assert (b[2], b[3]) == (0x0001, 0x2345) and b[4] == 1
    assert b[8] == (ord("a") << 8) | ord("b") and b[9] == (ord("/") << 8) | ord("c")
    assert b[24:28] == [7, DT_REAL, 0x3F80, 0x0000]
    assert b[28] == (ord("x") << 8) | ord(".") and b[29] == ord("y") << 8
    assert len(pin()) == 10


# ------------------------------------------------------------------ part 1
def run(d, args):
    r = d.read(T.REG_RESOURCE, 10)
    di, do, ai, vflag, vreg, retain, counter = r[3], r[4], r[5], r[6], r[7], r[8], r[9]
    print(f"Resource: DI={di} DO={do} AI={ai} VFLAG={vflag} VREG={vreg} RETAIN={retain} COUNTER={counter}")
    if vreg < 8 or vflag < 1:
        print("Need >= 8 VREG and >= 1 VFLAG tags")
        return None
    # Dense layout as in test_mbw.py: DI, DO, AI, VFLAG, VREG, RETAIN, COUNTER
    di0, do0 = 0, di
    ai0 = di + do
    vf0 = ai0 + ai
    vr0 = vf0 + vflag
    ct0 = vr0 + vreg + retain
    rule = T.encode_rule(T.TRG_INTERVAL, T.OP_NONE, T.ACT_INC, 0, vr0, for_ms=60000, action_param=1)
    A, B, C = vr0 + 1, vr0 + 2, vr0 + 3          # plain VREG targets (vr0 is the rule's counter)

    print("\n0. Probe")
    resp = d.write_regs(REG_SUB, [0])
    if resp is not None:
        check(False, "0x0D80 accepts a write: firmware built with SPLC_MQTT_SUB_ENABLE=1 (needs SPLC_MQTT_ENABLE=1)")
        print("  Build with SPLC_MQTT_SUB_ENABLE 1 in splcopts.h, clean rebuild, flash. Stopping.")
        return None
    check(True, "0x0D80 accepts a write (MQTT Subscribe present)")
    deploy_ok(d, rule, None, "Deploy (drops the probe draft)")
    caps, err = d.try_read(REG_CAPS, 2)
    if caps is None:
        check(False, f"0x0A08 readable (capabilities): {err}")
    else:
        val = (caps[0] << 16) | caps[1]
        check((val & CAP_MQTT_SUB) != 0, f"capability MQTT_SUB (0x20) set in 0x0A08 = 0x{val:08X}")

    print("\n1. Clean start: CLEAR_RULES")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e} (expect DONE/0)")
    check(all(v == 0 for v in read_sub(d)), "all 128 registers read 0 (every block disabled)")

    print("\n2. Draft invisible before COMMIT, byte-exact after it")
    b0 = block(qos=1, fifo=True, ign_retain=True, fs=FS_DEFAULT, timeout=30000,
               topic="factory/line1/+/cmd",
               pins=[pin(A, DT_INT32, 5, "setpoint"), pin(B, DT_REAL, 0x40200000, "a.b[1].c"),
                     pin(vf0, DT_BOOL, 1, "enable"), pin(C, DT_INT32, 0, "pos[2]")])
    b1 = block(qos=2, fs=FS_ZERO, timeout=0x00010000, topic="splc/#", pins=[pin(NONE, DT_INT32), pin(A, DT_INT32, 0, "raw")])
    want = {0: b0, 1: b1}
    check(write_blocks(d, want) is None, "FC16 of both blocks accepted")
    check(all(v == 0 for v in read_sub(d)), "read before COMMIT still returns the RUNNING blocks (empty)")
    exp = {}
    if deploy_ok(d, rule, None, "Deploy"):
        exp = dict(want)
    got = read_sub(d)
    check(got == image(exp), f"blocks read back byte-exact {TT.diff(got, image(exp)) or ''}")

    print("\n3. A Deploy that does not write the blocks keeps them")
    deploy_ok(d, rule, None, "Deploy without the blocks")
    check(read_sub(d) == image(exp), "blocks unchanged")

    print("\n4. FC06 changes one register, the rest of the block is kept")
    resp = d.fc06(REG_SUB + 3, 12000)         # block 0, fail-safe timeout low word
    check(not resp.isError(), "FC06 into the area accepted")
    check(read_sub(d) == image(exp), "before COMMIT the running blocks are unchanged")
    if deploy_ok(d, rule, None, "Deploy"):
        exp[0] = list(exp[0])
        exp[0][3] = 12000
    check(read_sub(d) == image(exp), "after COMMIT: the new timeout AND the rest of the block")

    print("\n5. COMMIT rejections are all-or-nothing, the draft is dropped")

    def bad(name, applicable=True, **kw):
        if not applicable:
            info(f"{name}: board has no such tag, skipped")
            return
        pins = kw.pop("pins", [pin(A, DT_INT32, 0, "k")])
        blk = block(pins=pins, **kw)
        good = block(topic="splc/test/good", pins=[pin(B, DT_INT32, 0, "g")])
        deploy_rejected(d, rule, {1: good, 0: blk}, name)
        check(read_sub(d) == image(exp), f"{name}: running blocks unchanged (the good block was not applied)")

    bad("QoS 3", qos=3)
    bad("fail-safe action 3", fs=3)
    bad("empty topic filter", topic="")
    bad("'#' not last", topic="a/#/b")
    bad("'#' not a whole level", topic="a/b#")
    bad("'+' not a whole level", topic="a+/b")
    bad("pin count 0", pins=[], count=0)
    bad("pin count 5", pins=[pin(A)] * 4, count=5)
    bad("target tag beyond the tags", pins=[pin(0x7FFF, DT_INT32, 0, "k")])
    bad("data type 0", pins=[pin(A, 0, 0, "k")])
    bad("data type 4", pins=[pin(A, 4, 0, "k")])
    bad("non-printable key", pins=[pin(A, DT_INT32, 0, "k\x01")])
    bad("second pin bad (first is fine)", pins=[pin(A, DT_INT32, 0, "k"), pin(A, 9, 0, "k")])
    deploy_ok(d, rule, None, "Deploy after the rejections")
    check(read_sub(d) == image(exp), "draft was dropped by the rejected COMMITs")

    print("\n5b. Accepted although unusual (decision 47: target kind is not restricted)")
    kinds = []
    if do > 0:
        kinds.append(("DO", do0))
    if di > 0:
        kinds.append(("DI", di0))
    if ai > 0:
        kinds.append(("AI", ai0))
    if counter > 0:
        kinds.append(("COUNTER", ct0))
    for name, tag in kinds:
        blk = block(topic=f"splc/test/{name.lower()}", pins=[pin(tag, DT_INT32, 0, "k")])
        if deploy_ok(d, rule, {1: blk}, f"target tag = {name}"):
            exp[1] = blk
        check(read_sub(d) == image(exp), f"{name} target stored as written")
    blk = block(topic="splc/test/skip", pins=[pin(NONE, DT_INT32, 0, "k"), pin(A, DT_INT32, 0, "j")])
    if deploy_ok(d, rule, {1: blk}, "a pin with target 0xFFFF (skipped)"):
        exp[1] = blk
    check(read_sub(d) == image(exp), "block with a skipped pin stored as written")
    junk = [0xABAB] * BLOCK_REGS
    junk[0] = 0
    if deploy_ok(d, rule, {1: junk}, "disabled block with garbage content"):
        exp[1] = junk
    check(read_sub(d) == image(exp), "the disabled block is stored as written (block 1 is off)")

    print("\n6. Address limits")
    last = read_sub(d)[SUB_REGS - 1]
    resp = d.fc16(REG_SUB + SUB_REGS - 1, [last, 0])
    check(resp.isError() and T.exc_code(resp) == 0x02, "FC16 running past 0x0DFF -> exception 0x02")
    check(read_sub(d) == image(exp), "...and nothing changed")
    check(not d.fc16(REG_SUB + SUB_REGS - 1, [last]).isError(), "FC16 of the very last register accepted")
    deploy_ok(d, rule, None, "Deploy (drops the draft)")
    check(read_sub(d) == image(exp), "blocks unchanged after the address tests")

    if args.reboot:
        print("\n7. Persistence across REBOOT and the clear commands")
        nd = d.reboot(args)
        if nd is None:
            check(False, "board came back after REBOOT")
            return None
        d = nd
        check(read_sub(d) == image(exp), "blocks identical after REBOOT (restored from Flash)")
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RETAIN)
        check(s == T.ST_DONE and e == 0, f"CLEAR_RETAIN result {s}/{e}")
        check(read_sub(d) == image(exp), "CLEAR_RETAIN keeps the blocks")
        for name, cmd in (("CLEAR_RULES", T.SYS_CLEAR_RULES), ("FACTORY_RESET", T.SYS_FACTORY)):
            deploy_ok(d, rule, exp, f"re-deploy the blocks before {name}")
            s, e = T.run_syscmd(d, cmd)
            check(s == T.ST_DONE and e == 0, f"{name} result {s}/{e}")
            check(all(v == 0 for v in read_sub(d)), f"{name} clears the blocks")
            nd = d.reboot(args)
            if nd is None:
                check(False, "board came back after REBOOT")
                return None
            d = nd
            check(all(v == 0 for v in read_sub(d)), f"...and they stay clear after REBOOT ({name} was saved)")
    else:
        print("\n7. (skipped: add --reboot for persistence and the clear commands)")

    print("\n8. Cleanup (CLEAR_RULES)")
    s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
    check(s == T.ST_DONE and e == 0, f"CLEAR_RULES result {s}/{e}")
    check(all(v == 0 for v in read_sub(d)), "blocks cleared")
    return d, rule, (A, B, C)


# ------------------------------------------------------------------ part 2
def make_publisher(host, port):
    import paho.mqtt.client as mqtt
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:                       # paho 1.x
        c = mqtt.Client()
    c.connect(host, port, 30)
    c.loop_start()
    return c


def wait_tags(d, want, timeout):
    """Poll {tag: value} until all match or the time runs out; returns the last read values."""
    t0 = time.time()
    while True:
        cur = {t: TT.tag_value(d, t) for t in want}
        if cur == want or time.time() - t0 > timeout:
            return cur
        time.sleep(0.3)


def publish_until(d, pub, topic, payload, want, tries=4, wait=2.5):
    """Publish `payload` repeatedly until the tags reach `want` (the first message can arrive before the
    board has (re)subscribed after the COMMIT). Returns the last tag values read."""
    cur = {}
    for _ in range(tries):
        pub.publish(topic, payload, qos=1).wait_for_publish(5)
        cur = wait_tags(d, want, wait)
        if cur == want:
            break
    return cur


def run_live(d, args, rule, tags):
    host, _, p = args.live.partition(":")
    port = int(p) if p else 1883
    A, B, C = tags
    topic = args.live_topic
    print(f"\n9. LIVE: receiving messages through broker {host}:{port}, topic {topic}")
    try:
        pub = make_publisher(host, port)
    except ImportError:
        check(False, "paho-mqtt installed (pip install paho-mqtt)")
        return
    except Exception as e:                       # noqa: BLE001
        check(False, f"PC can connect to the broker {host}:{port} ({e})")
        return
    check(True, f"PC connected to the broker {host}:{port}")
    try:
        j = block(qos=1, topic=topic, pins=[pin(A, DT_INT32, 0, "temp"), pin(B, DT_INT32, 0, "sp")])
        raw = block(qos=0, topic=topic + "/raw", pins=[pin(C, DT_INT32, 0, "")])
        if not deploy_ok(d, rule, {0: j, 1: raw}, "Deploy the live blocks"):
            return
        # known starting values (tags are plain VREGs; the engine only writes vr0)
        time.sleep(2.0)      # let the host (re)subscribe the new topics
        info("the board must be connected to the same broker; if every check below fails, read its log "
             "for 'subscribe' lines and the broker used")

        cur = publish_until(d, pub, topic, '{"temp": 42, "sp": 7}', {A: 42, B: 7})
        check(cur == {A: 42, B: 7}, f"JSON path keys: temp->tag {A}, sp->tag {B} (read {cur})")

        pub.publish(topic, '{"temp": 5}', qos=1).wait_for_publish(5)
        cur = wait_tags(d, {A: 5, B: 7}, 3.0)
        check(cur == {A: 42, B: 7}, f"atomic: a message missing 'sp' writes NOTHING (read {cur})")

        cur = publish_until(d, pub, topic, '{"TEMP": 8, "Sp": 9}', {A: 8, B: 9})
        check(cur == {A: 8, B: 9}, f"keys are case-insensitive and the next valid message applies (read {cur})")

        pub.publish(topic, "not json", qos=1).wait_for_publish(5)
        cur = wait_tags(d, {A: 1, B: 1}, 2.0)
        check(cur == {A: 8, B: 9}, f"a payload that is not JSON writes nothing (read {cur})")

        cur = publish_until(d, pub, topic + "/raw", "12.7", {C: 13}, tries=4)
        check(cur == {C: 13}, f"raw payload '12.7' into an INT32 tag is rounded to 13 (read {cur})")
        cur = publish_until(d, pub, topic + "/raw", "-3", {C: 0xFFFFFFFD}, tries=3)
        check(cur == {C: 0xFFFFFFFD}, f"raw payload '-3' stored as int32 (read 0x{cur.get(C, 0):08X})")
    finally:
        pub.loop_stop()
        pub.disconnect()
        s, e = T.run_syscmd(d, T.SYS_CLEAR_RULES)
        check(s == T.ST_DONE and e == 0, f"cleanup CLEAR_RULES result {s}/{e}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--baudrate", type=int, default=115200)
    p.add_argument("--chunk", type=int, default=48, help="registers per FC16 when writing the blocks (default 48)")
    p.add_argument("--reboot", action="store_true", help="also check persistence across REBOOT")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    p.add_argument("--live", default=None, metavar="HOST[:PORT]",
                   help="also test receiving messages: publish to this broker (needs paho-mqtt; the board "
                        "must be connected to the same broker)")
    p.add_argument("--live-topic", default="splc/test/sub", help="topic used by --live (default splc/test/sub)")
    R.add_args(p)
    args = p.parse_args()
    R.start(args)
    self_check_encoding()
    T.Dev.chunk = max(1, min(args.chunk, 120))

    client = T.make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    d = T.Dev(client, args.unit)
    link_error = None
    try:
        res = run(d, args)
        if res is not None and args.live:
            d2, rule, tags = res
            run_live(d2, args, rule, tags)
            d = d2
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
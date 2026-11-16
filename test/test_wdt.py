#!/usr/bin/env python3
"""
test_wdt.py - Watchdog supervisor of the Wi-Fi IO board (docs/SPLC_Watchdog_Design.md).

What it can prove from the PC, and what it cannot
-------------------------------------------------
The board has NO fault-injection hook, so this script cannot make the firmware hang by itself.
It checks everything that can be checked without one:

  1. BOOT REPORT   (needs the LOG UART)   after a REBOOT the boot log must show
                     "reset cause: ... SOFT=1 IWDG=0", the IWDG settings read back from CubeMX
                     (window = maximum, timeout ~ SPLC_WDG_HW_TIMEOUT_MS) and the supervisor line
                     (service / channel PLC / channel NET / boot grace = the values you expect),
                     and none of: "window mode is on", "differs by more than", "no kick function",
                     "no sign of life", "WATCHDOG RESET".
  2. REBOOT CYCLES                        --reboots N (default 2): the board comes back every time
                     and its uptime restarts; a soft reset must NOT be reported as a watchdog reset.
  3. SOAK                                 --soak MIN (default 2): reads DEVICE_HEALTH (0x0800) every
                     second; an uptime that goes backwards = an unexpected reset = FAIL (this is how a
                     spurious watchdog reset shows up). With the log port it also fails on any boot
                     banner, "no sign of life" or "WATCHDOG RESET" line during the soak.
  4. WATCH-TRIP                           --watch-trip SECONDS: passive. YOU cause a hang (see below),
                     the script times what happens: link lost, "channel X: no sign of life" line,
                     boot with IWDG=1, link back; and compares the timings with the design:
                         channel PLC trip  ~ SPLC_WDG_PLC_TIMEOUT_MS (+ service period) after the last answer
                         reset             <= SPLC_WDG_HW_TIMEOUT_MS after the trip line (+ boot-log latency)
                     How to cause the hang is not part of this script. Options: a temporary
                     test build with a hang inserted in the task you want to test, or any fault you
                     can produce on purpose. With a debugger attached and the core HALTED the IWDG is
                     frozen (SPLC_WDG_FREEZE_IN_DEBUG), so that does not reset the board.
  5. --selftest                           no hardware: runs the log parser and the timing evaluation
                     on built-in sample lines (also catches a log-format drift with the firmware).

Not covered (needs fault injection or the network): a hang with interrupts disabled, a stuck ST67
task (channel NET), a Flash erase longer than the grace, the bootloader button.

Setup
-----
    * board USB-CDC port for Modbus (this script's first argument), like the other test_*.py
    * (recommended) the board LOG UART (LPUART1, 115200) on a USB-serial adapter: --log-port COMx
    * firmware built with SPLC_WDG_ENABLE = 1 and the IWDG configured in CubeMX as in splcopts.h

Usage
-----
    python test_wdt.py --selftest
    python test_wdt.py COM14 --log-port COM30                       # boot report, 2 reboots, 2 min soak
    python test_wdt.py COM14 --log-port COM30 --reboots 5 --soak 30   # longer
    python test_wdt.py COM14 --log-port COM30 --reboots 0 --soak 0 --watch-trip 120 --expect-channel PLC
    python test_wdt.py COM14 --expect-hw-ms 8000 --expect-plc-ms 5000   # if you changed the options

Exit codes: 0 = all checks passed, 1 = a check failed, 2 = cannot open a port, 3 = link error.
This script does NOT write Flash (REBOOT only). Firmware status: not yet built or run on the board.
"""
import argparse
import re
import sys
import threading
import time

import test_report as R   # Excel evidence report (--report / --no-report)
import test_mqtt as T
from test_mqtt import check, info, LinkError

REG_HEALTH = 0x0800     # DEVICE_HEALTH, 10 registers, uptime_s is the first u32

# ------------------------------------------------------------------ log patterns
# The lines are produced by ports/splc_port/splc_wdt_port.c and services/plc_wdt/plc_wdt.c.
RE_RESET = re.compile(r"reset cause: IWDG=(\d) WWDG=(\d) SOFT=(\d) PIN=(\d) BOR=(\d) LPWR=(\d)")
RE_IWDG = re.compile(r"IWDG: prescaler /(\d+) reload (\d+) window 0x([0-9A-Fa-f]+) "
                     r"-> about (\d+) ms \(design value (\d+) ms\)")
RE_SUP = re.compile(r"supervisor: service (\d+) ms, channel PLC (\d+) ms, channel NET (\d+) ms, "
                    r"boot grace (\d+) ms")
RE_TRIP = re.compile(r"channel (\S+): no sign of life, (\d+) ms past its deadline")
RE_BOOT = re.compile(r"Wi-Fi IO board init start|reset cause:")
BAD_TEXT = {
    "window mode is on": "IWDG window mode is on (set the window to the maximum in CubeMX)",
    "differs by more than": "IWDG timeout differs from SPLC_WDG_HW_TIMEOUT_MS (fix CubeMX or the option)",
    "no kick function": "plc_wdt has no kick function registered",
}
WD_RESET_TEXT = "WATCHDOG RESET"
IWDG_WINDOW_DISABLE = 0xFFF


# ------------------------------------------------------------------ log capture
class LogCapture:
    """Reads the board log UART in a thread and keeps (monotonic time, line)."""

    def __init__(self, port, baud):
        import serial   # pyserial (already needed by pymodbus serial)
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.lines = []
        self._lock = threading.Lock()
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        buf = b""
        while not self._stop:
            try:
                chunk = self.ser.read(256)
            except Exception:
                time.sleep(0.2)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip("\r\x00 ")
                if line:
                    with self._lock:
                        self.lines.append((time.monotonic(), line))

    def mark(self):
        with self._lock:
            return len(self.lines)

    def since(self, mark):
        with self._lock:
            return list(self.lines[mark:])

    def wait_for(self, regex, mark, timeout):
        """First (time, line) after `mark` matching `regex` (str or compiled), or None."""
        rx = re.compile(regex) if isinstance(regex, str) else regex
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for t, line in self.since(mark):
                if rx.search(line):
                    return t, line
            time.sleep(0.1)
        return None

    def close(self):
        self._stop = True
        try:
            self.ser.close()
        except Exception:
            pass


# ------------------------------------------------------------------ pure evaluation (no I/O)
def uptime_from_regs(regs):
    """uptime_s from the first two registers of DEVICE_HEALTH.

    The word order on the wire is not confirmed (the struct is copied as it is in memory, the
    document says high word first). Below 65536 s one of the two words is 0, so the OR of both is
    the value in either order; above that the order matters and the little-endian one is used."""
    w0, w1 = regs[0] & 0xFFFF, regs[1] & 0xFFFF
    if w0 == 0 or w1 == 0:
        return w0 | w1
    return (w1 << 16) | w0


def check_boot_report(lines, exp, expect_iwdg_reset=False):
    """lines: list of log strings of ONE boot. exp: dict hw, service, plc, net, grace (ms).
    Returns a list of (ok, message)."""
    out = []
    text = "\n".join(lines)

    m = None
    for ln in lines:
        m = RE_RESET.search(ln)
        if m:
            break
    if m is None:
        out.append((False, "boot log has the 'reset cause:' line (is SPLC_WDG_ENABLE = 1 in this build?)"))
    else:
        iwdg, _wwdg, soft = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if expect_iwdg_reset:
            out.append((iwdg == 1, f"reset cause IWDG=1 (got IWDG={iwdg})"))
            out.append((WD_RESET_TEXT in text, "log says the previous run ended with a WATCHDOG RESET"))
        else:
            out.append((soft == 1 and iwdg == 0,
                        f"reset cause after REBOOT is SOFT=1 IWDG=0 (got SOFT={soft} IWDG={iwdg})"))
            out.append((WD_RESET_TEXT not in text, "a soft reboot is not reported as a WATCHDOG RESET"))

    m = None
    for ln in lines:
        m = RE_IWDG.search(ln)
        if m:
            break
    if m is None:
        out.append((False, "boot log has the 'IWDG: prescaler ...' line"))
    else:
        window = int(m.group(3), 16)
        est, design = int(m.group(4)), int(m.group(5))
        out.append((window == IWDG_WINDOW_DISABLE,
                    f"IWDG window is the maximum 0x{IWDG_WINDOW_DISABLE:X} (got 0x{window:X})"))
        out.append((design == exp["hw"], f"firmware SPLC_WDG_HW_TIMEOUT_MS = {exp['hw']} (got {design})"))
        lo, hi = exp["hw"] * 0.8, exp["hw"] * 1.25
        out.append((lo <= est <= hi, f"IWDG timeout estimated from CubeMX = {est} ms, expected "
                                     f"{exp['hw']} ms (-20%/+25%)"))

    m = None
    for ln in lines:
        m = RE_SUP.search(ln)
        if m:
            break
    if m is None:
        out.append((False, "boot log has the 'supervisor: service ...' line"))
    else:
        got = dict(service=int(m.group(1)), plc=int(m.group(2)), net=int(m.group(3)), grace=int(m.group(4)))
        for k in ("service", "plc", "net", "grace"):
            out.append((got[k] == exp[k], f"supervisor {k} = {exp[k]} ms (got {got[k]})"))

    for needle, why in BAD_TEXT.items():
        out.append((needle not in text, f"no '{needle}' in the boot log" + ("" if needle not in text else f": {why}")))
    trip = [ln for ln in lines if RE_TRIP.search(ln)]
    out.append((not trip, "no channel tripped during the boot" + ("" if not trip else f": {trip[0]}")))
    return out


def evaluate_trip(ev, exp, expect_channel, tol):
    """ev: dict of host times (seconds) and facts from watch_trip():
         t_last_ok   last successful Modbus read before the loss
         t_trip      time of the 'no sign of life' log line (or None)
         trip_ch     its channel name
         t_reset     time of the boot 'reset cause:' line (or None)
         iwdg_flag   IWDG flag of that line (0/1/None)
         t_back      time of the first successful read after the loss (or None)
       tol: dict  boot_log_s (latency from reset to the reset-cause line), margin_s.
       Returns (ok, message) list."""
    out = []
    out.append((ev.get("t_back") is not None, "the board answered again after the reset"))
    if ev.get("t_reset") is not None:
        out.append((ev.get("iwdg_flag") == 1, f"the new boot reports IWDG=1 (got {ev.get('iwdg_flag')})"))
    if expect_channel != "none":
        has = ev.get("t_trip") is not None
        out.append((has, f"a 'channel {expect_channel}: no sign of life' line was logged"))
        if has:
            out.append((ev.get("trip_ch") == expect_channel,
                        f"the tripped channel is {expect_channel} (got {ev.get('trip_ch')})"))
            if expect_channel == "PLC" and ev.get("t_last_ok") is not None:
                # The PLC task serves the Modbus link too, so the last answer is the hang start.
                d = ev["t_trip"] - ev["t_last_ok"]
                lo = exp["plc"] / 1000.0 - tol["poll_s"] - tol["margin_s"]
                hi = (exp["plc"] + exp["service"]) / 1000.0 + tol["poll_s"] + tol["margin_s"]
                out.append((lo <= d <= hi, f"trip came {d:.2f} s after the last answer "
                                           f"(expected {lo:.2f} .. {hi:.2f} s = PLC timeout + service period)"))
            if ev.get("t_reset") is not None:
                d = ev["t_reset"] - ev["t_trip"]
                hi = exp["hw"] / 1000.0 * 1.25 + tol["boot_log_s"] + tol["margin_s"]
                out.append((0.0 < d <= hi, f"'reset cause' line came {d:.2f} s after the trip line "
                                           f"(expected 0 .. {hi:.2f} s = IWDG timeout + boot-log latency)"))
    if ev.get("t_last_ok") is not None and ev.get("t_back") is not None:
        d = ev["t_back"] - ev["t_last_ok"]
        info(f"link was down for {d:.2f} s (last answer -> first answer after the reset)")
    return out


def report(results):
    """Print a list of (ok, message) through the shared check() so the failures are counted."""
    for ok, msg in results:
        check(ok, msg)
    return all(ok for ok, _ in results)


# ------------------------------------------------------------------ board access
def read_uptime(d):
    regs, err = d.try_read(REG_HEALTH, 2)
    if err is not None:
        return None, err
    return uptime_from_regs(regs), None


def reconnect(args, timeout):
    """Open the USB-CDC port again after a reset. Returns Dev or None."""
    port = args.port_after or args.port
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.5)
        try:
            c = T.make_client(port, args.baudrate)
            if not c.connect():
                continue
            nd = T.Dev(c, args.unit)
            nd.read(0x0000, 1)
            return nd
        except Exception:
            continue
    return None


def expectations(args):
    return dict(hw=args.expect_hw_ms, service=args.expect_service_ms, plc=args.expect_plc_ms,
                net=args.expect_net_ms, grace=args.expect_grace_ms)


def log_lines_text(entries):
    return [line for _, line in entries]


# ------------------------------------------------------------------ test steps
def step_link(d):
    print("\n1. Link and uptime")
    up1, err = read_uptime(d)
    if not check(err is None, f"DEVICE_HEALTH (0x{REG_HEALTH:04X}) is readable" + ("" if err is None else f" ({err})")):
        return False
    time.sleep(2.2)
    up2, err = read_uptime(d)
    check(err is None and up2 is not None and 1 <= up2 - up1 <= 4,
          f"uptime advances with real time ({up1} s -> {up2} s in about 2 s)")
    return True


def step_reboots(d, args, log):
    print(f"\n2. REBOOT cycles ({args.reboots}) with the boot report")
    exp = expectations(args)
    for i in range(args.reboots):
        print(f"  -- reboot {i + 1}/{args.reboots}")
        mark = log.mark() if log else 0
        t0 = time.monotonic()
        nd = d.reboot(args)
        if not check(nd is not None, "the board came back after REBOOT"):
            return None
        d = nd
        up, err = read_uptime(d)
        check(err is None and up is not None and up <= (time.monotonic() - t0) + 3,
              f"uptime restarted from 0 (now {up} s, {time.monotonic() - t0:.0f} s after the command)")
        if log:
            got = log.wait_for(RE_RESET, mark, 15.0)
            if not check(got is not None, "the boot report reached the log UART"):
                continue
            time.sleep(2.0)   # the supervisor / IWDG lines follow it by a few ms; let the rest arrive
            entries = log.since(mark)
            # one boot only: from the first boot marker after the command
            first = next((k for k, (_, ln) in enumerate(entries) if RE_BOOT.search(ln)), 0)
            report(check_boot_report(log_lines_text(entries[first:]), exp))
        else:
            info("no --log-port: the boot report is not checked")
    return d


def step_soak(d, args, log):
    print(f"\n3. Soak {args.soak} min (DEVICE_HEALTH every {args.poll:.1f} s)")
    mark = log.mark() if log else 0
    end = time.monotonic() + args.soak * 60.0
    last_up, last_ok, resets, fails, ok_n, max_gap = None, None, 0, 0, 0, 0.0
    t_report = time.monotonic() + 30.0
    while time.monotonic() < end:
        t = time.monotonic()
        up, err = read_uptime(d)
        if err is not None:
            fails += 1
            info(f"read failed: {err}")
        else:
            ok_n += 1
            if last_ok is not None:
                max_gap = max(max_gap, t - last_ok)
            last_ok = t
            if last_up is not None and up < last_up:
                resets += 1
                info(f"UPTIME WENT BACKWARDS ({last_up} s -> {up} s): the board reset during the soak")
            last_up = up
        if t >= t_report:
            info(f"{ok_n} reads ok, {fails} failed, uptime {last_up} s, longest gap {max_gap:.1f} s")
            t_report += 30.0
        time.sleep(max(0.0, args.poll - (time.monotonic() - t)))
    check(resets == 0, f"no unexpected reset during the soak (uptime never went backwards, {ok_n} reads)")
    check(fails <= args.max_link_errors, f"link errors during the soak: {fails} (allowed {args.max_link_errors})")
    check(max_gap <= args.max_gap_s, f"longest gap between two answers {max_gap:.1f} s (allowed {args.max_gap_s} s)")
    if log:
        lines = log_lines_text(log.since(mark))
        boots = [ln for ln in lines if RE_BOOT.search(ln)]
        trips = [ln for ln in lines if RE_TRIP.search(ln)]
        check(not boots, "no boot banner in the log during the soak" + ("" if not boots else f": {boots[0]}"))
        check(not trips, "no 'no sign of life' line during the soak" + ("" if not trips else f": {trips[0]}"))
        check(not any(WD_RESET_TEXT in ln for ln in lines), "no WATCHDOG RESET line during the soak")
    else:
        info("no --log-port: only the uptime / link checks were made")


def step_watch_trip(d, args, log):
    print(f"\n4. WATCH-TRIP: cause the hang NOW (waiting up to {args.watch_trip:.0f} s)")
    exp = expectations(args)
    mark = log.mark() if log else 0
    ev = dict(t_last_ok=None, t_trip=None, trip_ch=None, t_reset=None, iwdg_flag=None, t_back=None)
    start = time.monotonic()
    last_up = None
    lost = False
    while time.monotonic() - start < args.watch_trip:
        t = time.monotonic()
        if not lost:
            up, err = read_uptime(d)
            if err is None:
                if last_up is not None and up < last_up:
                    lost = True        # reset between two reads
                    ev["t_back"] = t
                else:
                    ev["t_last_ok"] = t
                last_up = up
            else:
                lost = True
        else:
            if ev["t_back"] is None:
                nd = reconnect(args, 1.5)
                if nd is not None:
                    d = nd
                    ev["t_back"] = time.monotonic()
        if log:
            for tt, ln in log.since(mark):
                m = RE_TRIP.search(ln)
                if m and ev["t_trip"] is None:
                    ev["t_trip"], ev["trip_ch"] = tt, m.group(1)
                    info(f"log: {ln}")
                m = RE_RESET.search(ln)
                if m and ev["t_reset"] is None:
                    ev["t_reset"], ev["iwdg_flag"] = tt, int(m.group(1))
                    info(f"log: {ln}")
        if lost and ev["t_back"] is not None and (not log or ev["t_reset"] is not None):
            break
        time.sleep(max(0.0, args.poll_fast - (time.monotonic() - t)))
    if not lost:
        check(False, "a hang / reset was seen (the link never stopped and the uptime never went backwards)")
        return d
    time.sleep(2.0)
    if log:
        entries = log.since(mark)
        first = next((k for k, (_, ln) in enumerate(entries) if RE_RESET.search(ln)), None)
        if first is not None:
            lines = log_lines_text(entries[first:])
            report([r for r in check_boot_report(lines, exp, expect_iwdg_reset=True)])
    tol = dict(boot_log_s=args.boot_log_s, margin_s=args.margin_s, poll_s=args.poll_fast)
    report(evaluate_trip(ev, exp, args.expect_channel, tol))
    return d


# ------------------------------------------------------------------ selftest (no hardware)
SAMPLE_BOOT = [
    "[INFO]BOARD_WIFI_IO : Wi-Fi IO board init start",
    "[INFO]WDT_PORT : reset cause: IWDG=0 WWDG=0 SOFT=1 PIN=0 BOR=0 LPWR=0",
    "[INFO]WDT_PORT : IWDG: prescaler /128 reload 1249 window 0xFFF -> about 5000 ms (design value 5000 ms)",
    "[INFO]WDT_PORT : supervisor: service 500 ms, channel PLC 2000 ms, channel NET 45000 ms, boot grace 30000 ms",
]
EXP = dict(hw=5000, service=500, plc=2000, net=45000, grace=30000)


def run_selftest():
    ok_all = True

    def t(name, cond):
        nonlocal ok_all
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        ok_all = ok_all and cond

    print("selftest (no hardware)")
    t("uptime: low word only", uptime_from_regs([123, 0]) == 123)
    t("uptime: high word only (other order)", uptime_from_regs([0, 123]) == 123)
    t("uptime: both words (little-endian order)", uptime_from_regs([0x0001, 0x0002]) == 0x20001)

    r = check_boot_report(SAMPLE_BOOT, EXP)
    t("good soft-reboot boot report passes", all(ok for ok, _ in r))

    bad = list(SAMPLE_BOOT)
    bad[1] = bad[1].replace("IWDG=0", "IWDG=1").replace("SOFT=1", "SOFT=0")
    t("a watchdog reset after a soft REBOOT is flagged", not all(ok for ok, _ in check_boot_report(bad, EXP)))

    bad = list(SAMPLE_BOOT)
    bad[2] = bad[2].replace("0xFFF", "0x100")
    t("IWDG window mode is flagged", not all(ok for ok, _ in check_boot_report(bad, EXP)))

    bad = list(SAMPLE_BOOT)
    bad[2] = bad[2].replace("about 5000 ms", "about 9000 ms")
    t("IWDG timeout far from the design value is flagged", not all(ok for ok, _ in check_boot_report(bad, EXP)))

    bad = list(SAMPLE_BOOT) + ["[ERROR]WDT_PORT : IWDG window mode is on: a kick before the window opens resets"]
    t("the firmware's own window-mode error line is flagged", not all(ok for ok, _ in check_boot_report(bad, EXP)))

    bad = list(SAMPLE_BOOT)
    bad[3] = bad[3].replace("channel PLC 2000", "channel PLC 3000")
    t("a different supervisor value is flagged", not all(ok for ok, _ in check_boot_report(bad, EXP)))

    t("a log without the reset-cause line is flagged",
      not all(ok for ok, _ in check_boot_report(SAMPLE_BOOT[:1] + SAMPLE_BOOT[2:], EXP)))

    wd = list(SAMPLE_BOOT)
    wd[1] = wd[1].replace("IWDG=0", "IWDG=1").replace("SOFT=1", "SOFT=0")
    wd.append("[WARN]WDT_PORT : the previous run ended with a WATCHDOG RESET")
    t("a real watchdog boot passes when IWDG=1 is expected",
      all(ok for ok, _ in check_boot_report(wd, EXP, expect_iwdg_reset=True)))

    tol = dict(boot_log_s=3.0, margin_s=0.5, poll_s=0.25)
    ev = dict(t_last_ok=100.0, t_trip=102.4, trip_ch="PLC", t_reset=106.0, iwdg_flag=1, t_back=108.0)
    t("PLC trip timeline inside the design passes",
      all(ok for ok, _ in evaluate_trip(ev, EXP, "PLC", tol)))
    ev2 = dict(ev, t_trip=108.0, t_reset=114.0)
    t("PLC trip 8 s after the last answer is flagged",
      not all(ok for ok, _ in evaluate_trip(ev2, EXP, "PLC", tol)))
    ev3 = dict(ev, t_reset=130.0)
    t("a reset far later than the IWDG timeout is flagged",
      not all(ok for ok, _ in evaluate_trip(ev3, EXP, "PLC", tol)))
    ev4 = dict(ev, t_trip=None, trip_ch=None)
    t("no trip line when one is expected is flagged",
      not all(ok for ok, _ in evaluate_trip(ev4, EXP, "PLC", tol)))
    t("hardware-only hang (no trip line) passes with --expect-channel none",
      all(ok for ok, _ in evaluate_trip(dict(ev4, t_reset=106.0), EXP, "none", tol)))

    print("\nSELFTEST PASS" if ok_all else "\nSELFTEST FAIL")
    return 0 if ok_all else 1


# ------------------------------------------------------------------ main
def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="board USB-CDC port (or tcp target), e.g. COM14")
    p.add_argument("--unit", type=int, default=1, help="Modbus unit of the board's config port (default 1)")
    p.add_argument("--baudrate", type=int, default=115200, help="baud of the USB-CDC port")
    p.add_argument("--port-after", default=None, help="COM port after a reboot if Windows renumbers it")
    p.add_argument("--log-port", default=None, help="board LOG UART (LPUART1) on a USB-serial adapter, e.g. COM30")
    p.add_argument("--log-baud", type=int, default=115200)
    p.add_argument("--reboots", type=int, default=2, help="REBOOT cycles with the boot report (default 2, 0 = skip)")
    p.add_argument("--soak", type=float, default=2.0, help="soak minutes (default 2, 0 = skip)")
    p.add_argument("--poll", type=float, default=1.0, help="soak read period in seconds (default 1)")
    p.add_argument("--max-link-errors", type=int, default=0, help="allowed failed reads during the soak (default 0)")
    p.add_argument("--max-gap-s", type=float, default=5.0, help="allowed longest gap between answers (default 5)")
    p.add_argument("--watch-trip", type=float, default=0.0, metavar="SECONDS",
                   help="passive mode: you cause a hang, the script times the reaction (0 = off)")
    p.add_argument("--expect-channel", choices=["PLC", "NET", "none"], default="PLC",
                   help="channel that should trip in --watch-trip; none = hang that stops the supervisor itself")
    p.add_argument("--poll-fast", type=float, default=0.25, help="read period in --watch-trip (default 0.25 s)")
    p.add_argument("--boot-log-s", type=float, default=3.0,
                   help="time from the reset to the 'reset cause' log line (default 3 s)")
    p.add_argument("--margin-s", type=float, default=0.5, help="timing tolerance in seconds (default 0.5)")
    p.add_argument("--expect-hw-ms", type=int, default=5000, help="SPLC_WDG_HW_TIMEOUT_MS (default 5000)")
    p.add_argument("--expect-service-ms", type=int, default=500, help="SPLC_WDG_SERVICE_MS (default 500)")
    p.add_argument("--expect-plc-ms", type=int, default=2000, help="SPLC_WDG_PLC_TIMEOUT_MS (default 2000)")
    p.add_argument("--expect-net-ms", type=int, default=45000, help="SPLC_WDG_NET_TIMEOUT_MS (default 45000)")
    p.add_argument("--expect-grace-ms", type=int, default=30000, help="SPLC_WDG_BOOT_GRACE_MS (default 30000)")
    p.add_argument("--selftest", action="store_true", help="no hardware: check the log parser and the timing rules")
    R.add_args(p)
    args = p.parse_args()

    if args.selftest:
        sys.exit(run_selftest())
    if not args.port:
        p.error("the board port is required (or use --selftest)")
    R.start(args)

    log = None
    if args.log_port:
        try:
            log = LogCapture(args.log_port, args.log_baud)
        except Exception as e:
            print(f"Cannot open the log port {args.log_port}: {e}")
            sys.exit(2)
    else:
        print("(no --log-port: the boot report and the trip lines cannot be checked, only uptime / link)")

    client = T.make_client(args.port, args.baudrate)
    if not client.connect():
        print(f"Cannot open {args.port}")
        sys.exit(2)
    d = T.Dev(client, args.unit)
    link_error = None
    try:
        if step_link(d):
            if args.reboots > 0:
                nd = step_reboots(d, args, log)
                if nd is not None:
                    d = nd
            if args.soak > 0:
                step_soak(d, args, log)
            if args.watch_trip > 0:
                d = step_watch_trip(d, args, log)
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
        if log:
            log.close()
    if link_error is not None:
        print(f"\nINCONCLUSIVE: a request outcome was unknown (link error). {T._failures} check(s) failed before it.")
        sys.exit(3)
    print("\nALL PASS" if T._failures == 0 else f"\n{T._failures} CHECK(S) FAILED")
    sys.exit(0 if T._failures == 0 else 1)


if __name__ == "__main__":
    main()
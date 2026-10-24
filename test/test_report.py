"""
test_report.py - shared Excel evidence report for the board test scripts.

What it does
------------
Every script in this folder prints its result as it runs:

    1. Clean start: CLEAR_RULES          <- section header (no indent)
      [PASS] CLEAR_RULES accepted        <- one check
      [FAIL] rule count == 0 (got 3)
      (info) ...

test_report taps stdout/stderr (the text still appears on the console as before), parses
those lines, and when the script ends writes ONE .xlsx file:

    Summary      verdict, totals (live COUNTIF formulas), per-section PASS/FAIL table
    Details      every check in order: section, description, PASS/FAIL, detail, seconds
    Environment  script, command line, arguments, port/baud/unit, git commit, versions
    Log          the complete console output, line by line

No check() function of any script has to change: the report is built from what the script
prints. The report is written even when the script fails, aborts on a link error or raises
(verdict FAIL / INCONCLUSIVE / ERROR), so the evidence is never lost.

Usage in a test script (3 lines)
--------------------------------
    import test_report as R              # next to the other imports
    R.add_args(p)                        # before p.parse_args(): adds --report / --no-report
    args = p.parse_args()
    R.start(args)                        # right after parse_args()

Command line (added to every script):
    --report FILE     write the report to FILE (default: reports/<script>_<YYYYmmdd_HHMMSS>.xlsx
                      next to the scripts)
    --no-report       do not write a report

Optional extras
---------------
    R.env("firmware", "7ed2909")         add a row to the Environment sheet
    R.check(cond, "text", expected=..., actual=...)    only for NEW scripts that want to log
                                         expected/actual values explicitly (prints the usual
                                         [PASS]/[FAIL] line, which the parser then picks up)

Needs `pip install openpyxl`. Without it a .csv with the same Details is written instead.

Run `python test_report.py --selftest` to produce a sample report without a board.
"""
import argparse
import atexit
import csv
import datetime as dt
import os
import platform
import re
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))

_RE_CHECK = re.compile(r"^\s*\[(PASS|FAIL)\]\s+(.*?)\s*$")
_RE_INFO = re.compile(r"^\s*(\(info\)|\[INFO\]|SKIP:|note:)", re.IGNORECASE)
_RE_HEADER_NUM = re.compile(r"^(\d+[.)]\s|\[[^\]]{1,12}\]\s|\*\s|---\s)")
_RE_DETAIL = re.compile(r"^(.*?)\s{3}\((.*)\)$")          # test_rule.py: "name   (detail)"
_RE_SUMMARY = re.compile(r"^\s*(ALL PASS|\d+ (CHECK\(S\) )?FAILED|\d+/\d+ checks passed|=+)\s*$")
_ILLEGAL = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f]")


def _clean(s):
    return _ILLEGAL.sub("", str(s))


# ---------------------------------------------------------------------------- recorder
class _Recorder:
    def __init__(self, script, args):
        self.script = script
        self.args = args
        self.t0 = time.time()
        self.started = dt.datetime.now()
        self.rows = []            # dicts: kind (CHECK/INFO), section, desc, result, detail, t
        self.log = []             # (t, stream, line)
        self.section = "(start)"
        self.prev_blank = True
        self.exit_code = None
        self.saw_error = False
        self.saw_link = False
        self.env_extra = []
        self.path = None
        self.written = False

    # called for every complete line printed by the script
    def feed(self, line, stream="out"):
        t = time.time() - self.t0
        line = _clean(line.rstrip("\r"))
        self.log.append((t, stream, line))
        blank = (line.strip() == "")
        m = _RE_CHECK.match(line)
        if m:
            desc, detail = m.group(2), ""
            d = _RE_DETAIL.match(desc)
            if d:
                desc, detail = d.group(1), d.group(2)
            self.rows.append(dict(kind="CHECK", section=self.section, desc=desc,
                                  result=m.group(1), detail=detail, t=t))
        elif "[LINK]" in line or line.startswith("INCONCLUSIVE"):
            self.saw_link = True
            self.rows.append(dict(kind="INFO", section=self.section, desc=line.strip(),
                                  result="INFO", detail="", t=t))
        elif "[ERROR]" in line or line.startswith("Traceback"):
            self.saw_error = True
            self.rows.append(dict(kind="INFO", section=self.section, desc=line.strip(),
                                  result="INFO", detail="", t=t))
        elif _RE_INFO.match(line):
            self.rows.append(dict(kind="INFO", section=self.section, desc=line.strip(),
                                  result="INFO", detail="", t=t))
        elif (not blank and not line[0].isspace() and not _RE_SUMMARY.match(line)
              and (self.prev_blank or _RE_HEADER_NUM.match(line))):
            self.section = line.strip()
        self.prev_blank = blank

    # ----- verdict
    def counts(self):
        p = sum(1 for r in self.rows if r["kind"] == "CHECK" and r["result"] == "PASS")
        f = sum(1 for r in self.rows if r["kind"] == "CHECK" and r["result"] == "FAIL")
        return p, f

    def verdict(self):
        p, f = self.counts()
        if f:
            return "FAIL"
        if self.saw_error:
            return "ERROR"
        if self.saw_link:
            return "INCONCLUSIVE"
        if self.exit_code not in (None, 0):
            return "ERROR" if p == 0 else "FAIL"
        return "PASS" if p else "NO RESULT"


class _Tee:
    def __init__(self, real, rec, stream):
        self._real, self._rec, self._stream, self._buf = real, rec, stream, ""

    def write(self, s):
        n = self._real.write(s)
        self._buf += s
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            self._rec.feed(line, self._stream)
        return n

    def flush(self):
        self._real.flush()

    def close_partial(self):
        if self._buf:
            self._rec.feed(self._buf, self._stream)
            self._buf = ""

    def __getattr__(self, name):
        return getattr(self._real, name)


_rec = None
_tees = []


# ---------------------------------------------------------------------------- public API
def add_args(parser):
    g = parser.add_argument_group("evidence report")
    g.add_argument("--report", default=None, metavar="FILE",
                   help="write the Excel report to FILE "
                        "(default: reports/<script>_<timestamp>.xlsx next to the scripts)")
    g.add_argument("--no-report", action="store_true", help="do not write an Excel report")


def start(args=None, **env):
    """Begin capturing. Call once, right after parse_args(). Safe to call twice."""
    global _rec
    if _rec is not None:
        return _rec
    if args is not None and getattr(args, "no_report", False):
        return None
    script = os.path.basename(sys.argv[0]) or "test"
    _rec = _Recorder(script, args)
    rep = getattr(args, "report", None) if args is not None else None
    if rep:
        _rec.path = rep
    else:
        stem = os.path.splitext(script)[0]
        stamp = _rec.started.strftime("%Y%m%d_%H%M%S")
        _rec.path = os.path.join(_HERE, "reports", f"{stem}_{stamp}.xlsx")
    for k, v in env.items():
        _rec.env_extra.append((k, v))
    for name in ("stdout", "stderr"):
        tee = _Tee(getattr(sys, name), _rec, "out" if name == "stdout" else "err")
        setattr(sys, name, tee)
        _tees.append((name, tee))

    real_exit = sys.exit

    def _exit(code=None):
        _rec.exit_code = 0 if code is None else (code if isinstance(code, int) else 1)
        real_exit(code)

    sys.exit = _exit
    atexit.register(_finish)
    return _rec


def env(key, value):
    """Add a row to the Environment sheet (e.g. firmware commit, board name)."""
    if _rec is not None:
        _rec.env_extra.append((key, value))


def check(cond, msg, expected=None, actual=None):
    """Optional helper for new scripts: prints the standard [PASS]/[FAIL] line with
    expected/actual appended as the detail the parser extracts."""
    ok = bool(cond)
    detail = ""
    if expected is not None or actual is not None:
        detail = f"   (expected {expected!r}, actual {actual!r})"
    print(f"  [{'PASS' if ok else 'FAIL'}] {msg}{detail}")
    return ok


# ---------------------------------------------------------------------------- writing
def _git_info():
    out = []
    try:
        root = subprocess.run(["git", "-C", _HERE, "rev-parse", "--show-toplevel"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
        if not root:
            return out
        def g(*a):
            return subprocess.run(["git", "-C", root, *a], capture_output=True, text=True,
                                  timeout=5).stdout.strip()
        out.append(("git commit", g("rev-parse", "--short", "HEAD")))
        out.append(("git branch", g("rev-parse", "--abbrev-ref", "HEAD")))
        out.append(("git subject", g("log", "-1", "--pretty=%s")))
        dirty = g("status", "--porcelain", "--untracked-files=no")
        out.append(("git working tree", "modified (uncommitted changes)" if dirty else "clean"))
    except Exception:
        pass
    return out


def _version(pkg):
    try:
        from importlib.metadata import version
        return version(pkg)
    except Exception:
        return "n/a"


def _env_rows(rec):
    rows = [("Script", rec.script),
            ("Command line", " ".join([os.path.basename(sys.argv[0])] + sys.argv[1:])),
            ("Started", rec.started.strftime("%Y-%m-%d %H:%M:%S")),
            ("Duration (s)", round(time.time() - rec.t0, 1))]
    if rec.args is not None:
        for k, v in sorted(vars(rec.args).items()):
            if k in ("report", "no_report"):
                continue
            rows.append((f"arg: {k}", "" if v is None else str(v)))
    rows += _git_info()
    rows += [("Host", platform.node()),
             ("OS", platform.platform()),
             ("Python", sys.version.split()[0]),
             ("pymodbus", _version("pymodbus")),
             ("pyserial", _version("pyserial")),
             ("openpyxl", _version("openpyxl"))]
    rows += rec.env_extra
    return [(_clean(k), _clean(v)) for k, v in rows]


def _write_xlsx(rec, path):
    from openpyxl import Workbook
    from openpyxl.formatting.rule import CellIsRule
    from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
    from openpyxl.utils import get_column_letter

    F = "Arial"
    f_norm, f_bold = Font(name=F, size=10), Font(name=F, size=10, bold=True)
    f_head = Font(name=F, size=10, bold=True, color="FFFFFF")
    f_title = Font(name=F, size=14, bold=True)
    fill_head = PatternFill("solid", start_color="1F3864")
    fill_label = PatternFill("solid", start_color="D9E1F2")
    thin = Side(style="thin", color="BFBFBF")
    box = Border(left=thin, right=thin, top=thin, bottom=thin)
    green = (PatternFill("solid", start_color="C6EFCE", end_color="C6EFCE"), Font(name=F, size=10, bold=True, color="006100"))
    red = (PatternFill("solid", start_color="FFC7CE", end_color="FFC7CE"), Font(name=F, size=10, bold=True, color="9C0006"))
    amber = (PatternFill("solid", start_color="FFEB9C", end_color="FFEB9C"), Font(name=F, size=10, bold=True, color="9C5700"))
    grey = Font(name=F, size=10, color="7F7F7F")

    wb = Workbook()
    ws_s = wb.active
    ws_s.title = "Summary"
    ws_d = wb.create_sheet("Details")
    ws_e = wb.create_sheet("Environment")
    ws_l = wb.create_sheet("Log")

    # ---- Details
    heads = ["#", "Section", "Description", "Result", "Detail", "t (s)"]
    for c, h in enumerate(heads, 1):
        cell = ws_d.cell(row=1, column=c, value=h)
        cell.font, cell.fill, cell.border = f_head, fill_head, box
        cell.alignment = Alignment(horizontal="center", vertical="center")
    n = 0
    for i, r in enumerate(rec.rows, start=2):
        if r["kind"] == "CHECK":
            n += 1
        vals = [n if r["kind"] == "CHECK" else None, _clean(r["section"]), _clean(r["desc"]),
                r["result"], _clean(r["detail"]), round(r["t"], 2)]
        for c, v in enumerate(vals, 1):
            cell = ws_d.cell(row=i, column=c, value=v)
            cell.font = grey if r["kind"] == "INFO" else f_norm
            cell.border = box
            cell.alignment = Alignment(vertical="top", wrap_text=c in (2, 3, 5))
        ws_d.cell(row=i, column=4).alignment = Alignment(horizontal="center", vertical="top")
    last = max(len(rec.rows) + 1, 2)
    for col, w in zip("ABCDEF", (6, 34, 78, 10, 42, 9)):
        ws_d.column_dimensions[col].width = w
    ws_d.freeze_panes = "A2"
    ws_d.auto_filter.ref = f"A1:F{last}"
    rng = f"D2:D{last}"
    ws_d.conditional_formatting.add(rng, CellIsRule(operator="equal", formula=['"PASS"'], fill=green[0], font=green[1]))
    ws_d.conditional_formatting.add(rng, CellIsRule(operator="equal", formula=['"FAIL"'], fill=red[0], font=red[1]))

    # ---- Summary
    p, f = rec.counts()
    ws_s["A1"] = f"Test report - {rec.script}"
    ws_s["A1"].font = f_title
    rows = [
        ("Verdict", rec.verdict(), None),
        ("Started", rec.started.strftime("%Y-%m-%d %H:%M:%S"), None),
        ("Duration (s)", round(time.time() - rec.t0, 1), "0.0"),
        ("Exit code", "" if rec.exit_code is None else rec.exit_code, None),
        ("Checks (total)", f'=COUNTIF(Details!$D$2:$D${last},"PASS")+COUNTIF(Details!$D$2:$D${last},"FAIL")', "0"),
        ("PASS", f'=COUNTIF(Details!$D$2:$D${last},"PASS")', "0"),
        ("FAIL", f'=COUNTIF(Details!$D$2:$D${last},"FAIL")', "0"),
        ("Pass rate", "=IFERROR(B8/B7,0)", "0.0%"),
    ]
    for i, (k, v, fmt) in enumerate(rows, start=3):
        a, b = ws_s.cell(row=i, column=1, value=k), ws_s.cell(row=i, column=2, value=v)
        a.font, a.fill, a.border = f_bold, fill_label, box
        b.font, b.border = f_norm, box
        b.alignment = Alignment(horizontal="left")
        if fmt:
            b.number_format = fmt
    ws_s["B3"].font = f_bold
    for word, st in (("PASS", green), ("FAIL", red), ("ERROR", red), ("INCONCLUSIVE", amber), ("NO RESULT", amber)):
        ws_s.conditional_formatting.add("B3", CellIsRule(operator="equal", formula=[f'"{word}"'], fill=st[0], font=st[1]))
    ws_s.conditional_formatting.add("B9", CellIsRule(operator="greaterThan", formula=["0"], fill=red[0], font=red[1]))

    r0 = 13
    ws_s.cell(row=r0 - 1, column=1, value="Per section").font = f_bold
    for c, h in enumerate(["Section", "PASS", "FAIL"], 1):
        cell = ws_s.cell(row=r0, column=c, value=h)
        cell.font, cell.fill, cell.border = f_head, fill_head, box
        cell.alignment = Alignment(horizontal="center")
    seen = []
    for r in rec.rows:
        if r["kind"] == "CHECK" and r["section"] not in seen:
            seen.append(r["section"])
    for i, sec in enumerate(seen, start=r0 + 1):
        ws_s.cell(row=i, column=1, value=_clean(sec))
        ws_s.cell(row=i, column=2, value=f'=SUMPRODUCT((Details!$B$2:$B${last}=$A{i})*(Details!$D$2:$D${last}="PASS"))')
        ws_s.cell(row=i, column=3, value=f'=SUMPRODUCT((Details!$B$2:$B${last}=$A{i})*(Details!$D$2:$D${last}="FAIL"))')
        for c in (1, 2, 3):
            ws_s.cell(row=i, column=c).font = f_norm
            ws_s.cell(row=i, column=c).border = box
        ws_s.cell(row=i, column=1).alignment = Alignment(wrap_text=True, vertical="top")
    if seen:
        end = r0 + len(seen)
        ws_s.conditional_formatting.add(f"C{r0 + 1}:C{end}", CellIsRule(operator="greaterThan", formula=["0"], fill=red[0], font=red[1]))
    ws_s.column_dimensions["A"].width = 64
    ws_s.column_dimensions["B"].width = 22
    ws_s.column_dimensions["C"].width = 10

    # ---- Environment
    for c, h in enumerate(["Item", "Value"], 1):
        cell = ws_e.cell(row=1, column=c, value=h)
        cell.font, cell.fill, cell.border = f_head, fill_head, box
    for i, (k, v) in enumerate(_env_rows(rec), start=2):
        a, b = ws_e.cell(row=i, column=1, value=k), ws_e.cell(row=i, column=2, value=v)
        a.font, a.fill, a.border = f_bold, fill_label, box
        b.font, b.border = f_norm, box
        b.alignment = Alignment(wrap_text=True, vertical="top")
    ws_e.column_dimensions["A"].width = 24
    ws_e.column_dimensions["B"].width = 90
    ws_e.freeze_panes = "A2"

    # ---- Log
    for c, h in enumerate(["t (s)", "Stream", "Line"], 1):
        cell = ws_l.cell(row=1, column=c, value=h)
        cell.font, cell.fill, cell.border = f_head, fill_head, box
    for i, (t, s, line) in enumerate(rec.log, start=2):
        ws_l.cell(row=i, column=1, value=round(t, 2)).font = grey
        ws_l.cell(row=i, column=2, value="stderr" if s == "err" else "stdout").font = grey
        ws_l.cell(row=i, column=3, value=line).font = Font(name="Consolas", size=9)
    ws_l.column_dimensions["A"].width = 9
    ws_l.column_dimensions["B"].width = 9
    ws_l.column_dimensions["C"].width = 140
    ws_l.freeze_panes = "A2"

    wb.save(path)


def _write_csv(rec, path):
    with open(path, "w", newline="", encoding="utf-8-sig") as fh:
        w = csv.writer(fh)
        w.writerow(["#", "Section", "Description", "Result", "Detail", "t (s)"])
        n = 0
        for r in rec.rows:
            if r["kind"] == "CHECK":
                n += 1
            w.writerow([n if r["kind"] == "CHECK" else "", r["section"], r["desc"],
                        r["result"], r["detail"], round(r["t"], 2)])


def _finish():
    rec = _rec
    if rec is None or rec.written:
        return
    rec.written = True
    for name, tee in _tees:
        tee.close_partial()
        setattr(sys, name, tee._real)
    path = rec.path
    try:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        try:
            _write_xlsx(rec, path)
        except ImportError:
            path = os.path.splitext(path)[0] + ".csv"
            _write_csv(rec, path)
            print("(report) openpyxl is not installed (pip install openpyxl): wrote CSV instead")
        p, f = rec.counts()
        print(f"\nReport: {path}   [{rec.verdict()}  PASS={p}  FAIL={f}]")
    except Exception as e:                       # never let the report hide the test result
        print(f"\n(report) could not write the report: {type(e).__name__}: {e}")


# ---------------------------------------------------------------------------- self-test
def _selftest(path):
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    add_args(ap)
    a = ap.parse_args(["COM99", "--report", path])
    a.unit, a.baudrate = 1, 115200
    start(a)
    env("board", "selftest (no hardware)")
    print("\n1. Clean start: CLEAR_RULES")
    print("  [PASS] CLEAR_RULES accepted")
    print("  [PASS] rule count == 0")
    print("\n2. Draft invisible before COMMIT")
    print("  [PASS] draft not visible")
    print("  [FAIL] block byte-exact after COMMIT   (expected 0x1234, got 0x1235)")
    print("  (info) example info line")
    print("\n[R] reboot persistence")
    check(True, "value survives reboot", expected=7, actual=7)
    check(False, "retain counter", expected=3, actual=2)
    print("\n* 3. group with wildcard chars?")
    print("  [PASS] section name starting with * and ? must not break the counts")
    print("\n1 CHECK(S) FAILED")
    sys.exit(1)


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        i = sys.argv.index("--selftest")
        out = sys.argv[i + 1] if i + 1 < len(sys.argv) else "selftest_report.xlsx"
        _selftest(out)
    else:
        print(__doc__)
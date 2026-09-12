#!/usr/bin/env python3
"""Validate IKOS's IEEE-754 work against InterflopBench.

InterflopBench (https://github.com/interflop/InterflopBench) ships 93 small C
programs, each with a `metadata.json` listing concrete input vectors and the
floating-point error classes every vector actually triggers on real hardware
(cancellation, overflow, underflow, nan, div_zero, comparison, no_error).

Most of those classes are questions about *precision* that a static analyzer
cannot answer. Three of them are questions about *special values*, and those IKOS
can answer. This harness answers them in two tiers.

  Tier 1 -- robustness. Run the whole suite through the FP pipeline and look for
            crashes, traps and timeouts. This is the load-bearing check for the
            "total intrinsic switch" work: an unhandled float intrinsic traps.

  Tier 2 -- soundness and precision of the FP abstract domain. For every
            benchmark with a one-argument scalar kernel, generate a driver that
            calls the kernel with each concrete dataset vector and asserts two
            properties with __ikos_assert:

              nan_free : y == y
              finite : -MAX <= y <= MAX

            Both are decidable from the FP domain alone. Comparing IKOS's verdict
            against the recorded ground truth classifies every case:

              ground truth FALSE + IKOS error   -> DETECTED     (good)
              ground truth FALSE + IKOS warning -> MISSED       (sound, imprecise)
              ground truth FALSE + IKOS ok      -> UNSOUND      (proved a lie)
              ground truth TRUE  + IKOS error   -> UNSOUND      (refuted truth)
              ground truth TRUE  + IKOS ok      -> CORRECT      (good)

            UNSOUND is the only category that is a real bug in either direction.
            Everything else is a coverage or precision measurement.

Scope limits, stated up front:
  * `cancellation`, `comparison` and `underflow` are NOT assert-checked. They
    change accuracy, not the special-value structure, so both properties hold
    for them and IKOS has nothing to say.
  * `div_zero` is informational only. IKOS's `dbz` checker is integer-only, so
    a float divide-by-zero is never reported. Recorded as a coverage gap.
  * Only 16 float intrinsics are modeled (sqrt, log, log2, log10, pow, fabs,
    floor, ceil, trunc, round, rint, copysign, maxnum, minnum, fma, fmuladd).
    sin/cos/tan/asin/acos/exp/expm1/tanh/log1p/fmod/tgamma/... are opaque extern
    calls, so any kernel using them returns top and can only ever be MISSED.
    Each benchmark is tagged MODELED or OPAQUE so the two are not conflated.

Usage:
  ./run_interflopbench.py [--bench-dir DIR] [--ikos PATH] [--tier 1|2|all]
                          [--report report.json] [--jobs N]

The benchmark repo is not vendored. Fetch it once with:
  git clone --filter=blob:none --sparse \\
      https://github.com/interflop/InterflopBench ~/.cache/interflopbench
  git -C ~/.cache/interflopbench sparse-checkout set examples
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

# Clang flags for the three-stage pipeline. Taken from libruntest.py so the two
# cannot drift; that file is the source of truth for how ikos wants its input
# compiled. Fall back to a copy only if it is not importable.
_REGRESSION_DIR = os.path.join(REPO_ROOT, "analyzer", "test", "regression")
try:
    sys.path.insert(0, _REGRESSION_DIR)
    import libruntest as _lrt
    EMIT_LLVM_FLAGS = _lrt.clang_emit_llvm_flags()
    IKOS_CLANG_FLAGS = _lrt.clang_ikos_flags()
    sys.path.remove(_REGRESSION_DIR)
except Exception:  # pragma: no cover - fallback keeps the harness standalone
    EMIT_LLVM_FLAGS = ["-c", "-emit-llvm"]
    IKOS_CLANG_FLAGS = ["-Wall", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0",
                       "-D__IKOS__", "-g", "-O0",
                       "-Xclang", "-disable-O0-optnone"]

# Ground-truth error classes that make `nan_free` false, and that make `finite`
# false. NaN is not finite either, so it appears in both sets: `NaN >= -MAX`
# is false, so a NaN result refutes the finiteness assertion too. Everything
# else leaves both properties true.
NAN_ERRORS = {"nan"}
INF_ERRORS = {"overflow", "nan"}
# Classes IKOS has no reporter for at all; counted as coverage gaps, not misses.
UNREPORTABLE = {"cancellation", "comparison", "underflow", "div_zero"}

# Float intrinsics IKOS actually models (see frontend/llvm/src/import/bundle.cpp).
MODELED_MATH = {
    "sqrt", "log", "log2", "log10", "pow", "fabs", "floor", "ceil", "trunc",
    "round", "rint", "copysign", "fmax", "fmin", "fma", "fmuladd",
}

FLT_MAX = "3.4028234663852886e+38F"
DBL_MAX = "1.7976931348623157e+308"

# checks.status, from analyzer/test/regression/libruntest.py
STATUS = {0: "ok", 1: "warning", 2: "error", 3: "unreachable"}
# CheckerName enum order in analyzer/include/ikos/analyzer/checker/name.hpp
F2I_CHECKER = 17   # FloatToIntOverflow
DBZ_CHECKER = 1    # DivisionByZero

TIMEOUT = 180


def eprint(*a):
    print(*a, file=sys.stderr)


# ---------------------------------------------------------------------------
# Benchmark discovery
# ---------------------------------------------------------------------------

KERNEL_DEF = re.compile(
    r"(?m)^\s*(float|double|REAL|long double)\s*\n\s*kernel\s*\(([^)]*)\)"
)
KERNEL_DEF_INLINE = re.compile(
    r"(?m)^(float|double|REAL|long double)\s+kernel\s*\(([^)]*)\)"
)
REAL_TYPEDEF = re.compile(r"typedef\s+([A-Za-z_][A-Za-z_0-9 ]*?)\s+REAL\s*;")
MATH_CALL = re.compile(r"\b([a-z][a-z0-9_]*)\s*\(")


class Bench:
    def __init__(self, name, path):
        self.name = name
        self.path = path
        self.src = open(os.path.join(path, "bench.c")).read()
        with open(os.path.join(path, "metadata.json")) as f:
            self.meta = json.load(f)
        self.inputs = []  # list of (input_dict, errors)
        for ds in self.meta.get("datasets", []):
            for inp in ds.get("inputs", []):
                self.inputs.append((inp, set(ds.get("errors", []))))
        self.math_used = sorted(
            m for m in set(MATH_CALL.findall(self.src))
            if m not in ("if", "for", "while", "return", "sizeof", "kernel",
                        "main", "printf", "scanf", "assert", "atoi", "atof",
                        "malloc", "free", "fabsf", "sqrtf")
        )
        self.modeled = all(m in MODELED_MATH for m in self.math_used)
        self.all_errors = set()
        for _, e in self.inputs:
            self.all_errors |= e
        self.tier2 = self._tier2_signature()

    def _tier2_signature(self):
        """Return (ctype, argname) for a 1-arg scalar kernel, else None."""
        m = KERNEL_DEF.search(self.src) or KERNEL_DEF_INLINE.search(self.src)
        if not m:
            return None
        ret, raw_args = m.group(1), m.group(2)
        args = [a.strip() for a in raw_args.split(",") if a.strip()]
        if len(args) != 1:
            return None
        td = REAL_TYPEDEF.search(self.src)
        real = td.group(1).strip() if td else None
        resolve = {"float": "float", "double": "double", "REAL": real}
        ctype = resolve.get(ret)
        atype = resolve.get(args[0].split()[0])
        if ctype not in ("float", "double") or atype not in ("float", "double"):
            return None
        if len(self.meta.get("ordered_inputs", [])) != 1:
            return None
        return ctype


def build_tool(args, repo_root):
    """Resolve how to run the pipeline. See run_ikos for the two modes."""
    t = object.__new__(_Tool)
    three = [args.clang, args.ikos_pp, args.ikos_analyzer]
    if any(three):
        if not all(three):
            eprint("--clang, --ikos-pp and --ikos-analyzer must be given together")
            sys.exit(2)
        t.mode = "three-stage"
        # Every pipeline command runs with cwd set to a per-benchmark temp dir,
        # so relative paths handed in on the command line would not resolve.
        t.ikos_pp = os.path.abspath(args.ikos_pp)
        t.ikos_analyzer = os.path.abspath(args.ikos_analyzer)
        t.clang_flags = [os.path.abspath(args.clang)] + IKOS_CLANG_FLAGS + EMIT_LLVM_FLAGS
    elif os.path.exists(args.ikos):
        t.mode = "wrapper"
        t.ikos = os.path.abspath(args.ikos)
    else:
        eprint("ikos not found: %s" % args.ikos)
        sys.exit(2)
    return t


class _Tool:
    pass


def discover(bench_dir):
    out = []
    for name in sorted(os.listdir(bench_dir)):
        p = os.path.join(bench_dir, name)
        if not os.path.isdir(p):
            continue
        if not os.path.exists(os.path.join(p, "bench.c")):
            continue
        if not os.path.exists(os.path.join(p, "metadata.json")):
            continue
        out.append(Bench(name, p))
    return out


# ---------------------------------------------------------------------------
# IKOS plumbing
# ---------------------------------------------------------------------------

def _first_error_line(text):
    """First line that actually explains a failure, for the report."""
    for ln in text.splitlines():
        if "error" in ln.lower():
            return ln.strip()[:160]
    return text.strip().splitlines()[-1][:160] if text.strip() else ""


def run_ikos(tool, source, analyses, workdir):
    """Run the analysis pipeline over `source` and return the result.

    Two modes, depending on what `tool` was built from:

      wrapper      --ikos <install/bin/ikos>. One command, needs the installed
                   python module.
      three-stage  --clang/--ikos-pp/--ikos-analyzer. Mirrors what
                   libruntest.py does so the harness runs straight out of the
                   build tree with no `cmake --install` step, which is what
                   ctest needs. Flag sets are imported from libruntest rather
                   than copied, so they cannot drift.
    """
    db = os.path.join(workdir, "out.db")
    if os.path.exists(db):
        os.remove(db)

    if tool.mode == "wrapper":
        cmds = [[tool.ikos, source, "-a", ",".join(analyses), "-o", db]]
    else:
        bc = os.path.join(workdir, "in.bc")
        pp = os.path.join(workdir, "in.pp.bc")
        cmds = [
            tool.clang_flags + [source, "-o", bc],
            [tool.ikos_pp, "-opt=basic", bc, "-o", pp],
            [tool.ikos_analyzer, "-a=%s" % ",".join(analyses), "-d=interval",
             "-entry-points=main", "-proc=inter", pp, "-o=" + db],
        ]

    out, err = "", ""
    for cmd in cmds:
        try:
            p = subprocess.run(cmd, cwd=workdir, capture_output=True, text=True,
                              timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return {"status": "timeout", "db": None, "stderr": err, "stdout": out}
        except OSError as e:
            return {"status": "missing-tool", "db": None,
                    "stderr": err + "\n" + str(e), "stdout": out}
        out += p.stdout or ""
        err += p.stderr or ""
        if p.returncode != 0:
            return {"status": "error", "db": None, "stderr": err, "stdout": out}
    return {"status": "ok", "db": db, "stderr": err, "stdout": out}


def line_statuses(db):
    """Map source line -> list of check statuses (as strings)."""
    if not db or not os.path.exists(db):
        return {}
    con = sqlite3.connect(db)
    cur = con.cursor()
    out = {}
    try:
        cur.execute(
            "SELECT statements.line, checks.status FROM checks "
            "INNER JOIN statements ON checks.statement_id = statements.id"
        )
        for line, st in cur.fetchall():
            out.setdefault(int(line), []).append(STATUS.get(st, "?%s" % st))
    except sqlite3.Error:
        pass
    con.close()
    return out


def f2i_counts(db, checker):
    if not db or not os.path.exists(db):
        return {}
    con = sqlite3.connect(db)
    cur = con.cursor()
    out = {}
    try:
        cur.execute("SELECT status, COUNT(*) FROM checks WHERE checker=? "
                   "GROUP BY status", (checker,))
        for st, n in cur.fetchall():
            out[STATUS.get(st, "?%s" % st)] = n
    except sqlite3.Error:
        pass
    con.close()
    return out


# ---------------------------------------------------------------------------
# Tier 1: robustness over the whole suite
# ---------------------------------------------------------------------------

def tier1(bench, tool):
    wd = tempfile.mkdtemp(prefix="ifb1_")
    try:
        shutil.copy(os.path.join(bench.path, "bench.c"), wd)
        r = run_ikos(tool, "bench.c", ["f2i", "prover", "dbz"], wd)
        status = r["status"]
        # A benchmark that will not compile on this host is a benchmark problem,
        # not an analyzer problem. 33_subnormal_func1 includes <xmmintrin.h>,
        # which hard-errors on arm64. Keep it out of the robustness denominator.
        if status == "error" and ("error while compiling" in r["stderr"] or
                                "errors generated" in r["stderr"]):
            status = "compile-error"
        res = {"bench": bench.name, "status": status,
               "f2i": f2i_counts(r["db"], F2I_CHECKER),
               "dbz": f2i_counts(r["db"], DBZ_CHECKER),
               "truth": sorted(bench.all_errors),
               "detail": _first_error_line(r["stderr"]) if status != "ok" else ""}
        low = r["stderr"].lower()
        # Genuine crash markers only. Note "unreachable" alone is useless: the
        # normal summary line reads "Total number of unreachable checks".
        res["trap"] = any(s in low for s in
                         ("ikos_unreachable", "sigtrap", "segmentation fault",
                          "core dumped", "terminate called"))
        return res
    finally:
        shutil.rmtree(wd, ignore_errors=True)


# ---------------------------------------------------------------------------
# Tier 2: per-input soundness / precision
# ---------------------------------------------------------------------------

DRIVER_HEAD = """\
/* Generated by run_interflopbench.py -- do not edit. */
#define main interflop_unused_main
#include "{bench_c}"
#undef main

extern void __ikos_assert(int);
"""


def gen_driver(bench, out_path):
    ctype = bench.tier2
    maxv = FLT_MAX if ctype == "float" else DBL_MAX
    lines = DRIVER_HEAD.format(bench_c=os.path.join(bench.path, "bench.c")).split("\n")
    plan = []  # (case_idx, input, errors, nan_line, fin_line)
    for i, (inp, errs) in enumerate(bench.inputs):
        val = list(inp.values())[0]
        lit = repr(float(val)) + ("F" if ctype == "float" else "")
        lines.append("void case_%d(void) {" % i)
        lines.append("  %s y = kernel(%s);" % (ctype, lit))
        nan_line = len(lines) + 1
        lines.append("  __ikos_assert(y == y);")
        fin_line = len(lines) + 1
        lines.append("  __ikos_assert(y >= -%s && y <= %s);" % (maxv, maxv))
        lines.append("}")
        lines.append("")
        plan.append((i, inp, errs, nan_line, fin_line))
    lines.append("int main(void) {")
    for i in range(len(bench.inputs)):
        lines.append("  case_%d();" % i)
    lines.append("  return 0;")
    lines.append("}")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return plan


def classify(truth_false, verdict):
    """Label one (property, ground truth, IKOS verdict) triple.

    truth_false -- the property does NOT hold on real hardware for this vector.

      truth FALSE + error       DETECTED            IKOS refuted it. Good.
      truth FALSE + warning     MISSED              Sound, no precision.
      truth FALSE + ok        UNSAT_FALSE_PROVEN    IKOS proved a lie. Bug.
      truth TRUE  + error     UNSAT_TRUE_REFUTED    IKOS refuted truth. Bug.
      truth TRUE  + warning   IMPRECISE             Sound, no precision.
      truth TRUE  + ok        CORRECT               Good.
      any         + unreachable UNREACHABLE_CLAIM   IKOS says the code cannot run.
      any         + missing     NO_CHECK            No check emitted at all.
    """
    if verdict == "unreachable":
        return "UNREACHABLE_CLAIM"
    if verdict == "missing":
        return "NO_CHECK"
    if truth_false:
        if verdict == "error":
            return "DETECTED"
        if verdict == "ok":
            return "UNSAT_FALSE_PROVEN"
        return "MISSED"
    if verdict == "error":
        return "UNSAT_TRUE_REFUTED"
    if verdict == "ok":
        return "CORRECT"
    return "IMPRECISE"


def tier2(bench, tool, keep_dir=None):
    wd = keep_dir or tempfile.mkdtemp(prefix="ifb2_")
    drv = os.path.join(wd, "%s_drv.c" % bench.name)
    plan = gen_driver(bench, drv)
    r = run_ikos(tool, os.path.basename(drv), ["prover"], wd)
    cases = []
    if r["status"] != "ok":
        return {"bench": bench.name, "status": r["status"],
                "stderr": r["stderr"][-2000:], "cases": cases}
    st = line_statuses(r["db"])

    def at(line):
        s = st.get(line, [])
        if not s:
            return "missing"
        # Worst-case verdict across any check recorded on that line.
        for want in ("error", "warning", "unreachable", "ok"):
            if want in s:
                return want
        return s[0]

    for i, inp, errs, nan_line, fin_line in plan:
        nan_v = at(nan_line)
        fin_v = at(fin_line)
        nan_false = bool(errs & NAN_ERRORS)
        fin_false = bool(errs & INF_ERRORS)
        cases.append({
            "input": inp,
            "errors": sorted(errs),
            "nan_free": {"truth": (not nan_false), "ikos": nan_v,
                        "label": classify(nan_false, nan_v)},
            "finite": {"truth": (not fin_false), "ikos": fin_v,
                      "label": classify(fin_false, fin_v)},
        })
    if keep_dir is None:
        shutil.rmtree(wd, ignore_errors=True)
    return {"bench": bench.name, "status": "ok", "modeled": bench.modeled,
            "math_used": bench.math_used, "cases": cases}


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def tally(results, key):
    t = {}
    for b in results:
        for c in b.get("cases", []):
            lab = c[key]["label"]
            t[lab] = t.get(lab, 0) + 1
    return t


def print_scorecard(tier1_res, tier2_res):
    print("=" * 72)
    print("TIER 1 -- robustness (%d benchmarks through the FP pipeline)" %
          len(tier1_res))
    print("=" * 72)
    by = {}
    for r in tier1_res:
        k = r["status"] + (" +TRAP" if r["trap"] else "")
        by[k] = by.get(k, 0) + 1
    for k, v in sorted(by.items()):
        print("  %-18s %d" % (k, v))
    for r in tier1_res:
        if r["status"] != "ok":
            print("  !! %-26s %s  %s" % (r["bench"], r["status"], r.get("detail", "")))
    f2i_any = [r for r in tier1_res if r["f2i"]]
    print("  f2i reports: %d benchmarks" % len(f2i_any))
    for r in f2i_any:
        print("     %-32s %s" % (r["bench"], r["f2i"]))

    # div_zero coverage: IKOS's `dbz` checker is integer-only, so a float
    # divide-by-zero should produce zero reports. Verify rather than assume.
    dz = [r for r in tier1_res if "div_zero" in r.get("truth", [])]
    caught = [r for r in dz if r["dbz"].get("error") or r["dbz"].get("warning")]
    print("  div_zero ground truth: %d benchmarks; dbz reported on %d"
          % (len(dz), len(caught)))
    if not caught:
        print("     -> confirms the gap: no float divide-by-zero is ever reported")

    print()
    print("=" * 72)
    print("TIER 2 -- soundness / precision (%d benchmarks, %d input vectors)" %
          (len(tier2_res), sum(len(b.get("cases", [])) for b in tier2_res)))
    print("=" * 72)
    for prop in ("nan_free", "finite"):
        t = tally(tier2_res, prop)
        print("  %s:" % prop)
        for k in ("CORRECT", "DETECTED", "MISSED", "IMPRECISE",
                 "UNSAT_FALSE_PROVEN", "UNSAT_TRUE_REFUTED"):
            if k in t:
                print("    %-20s %d" % (k, t[k]))
    unsound = []
    for b in tier2_res:
        for c in b.get("cases", []):
            for prop in ("nan_free", "finite"):
                if c[prop]["label"].startswith("UNSAT"):
                    unsound.append((b["bench"], c["input"], prop, c[prop]))
    print()
    if unsound:
        print("  UNSOUND RESULTS (%d) -- these are bugs:" % len(unsound))
        for n, inp, prop, d in unsound[:40]:
            print("    %-30s %-24s %-9s truth=%s ikos=%s" %
                  (n, json.dumps(inp), prop, d["truth"], d["ikos"]))
        if len(unsound) > 40:
            print("    ... %d more" % (len(unsound) - 40))
    else:
        print("  No unsound results.")

    # Split precision by whether every math call is modeled.
    for prop in ("nan_free", "finite"):
        m = tally([b for b in tier2_res if b.get("modeled")], prop)
        u = tally([b for b in tier2_res if not b.get("modeled")], prop)
        print("  %s  [all-math-modeled vs opaque-libm]: modeled=%s opaque=%s" %
              (prop, json.dumps(m, sort_keys=True), json.dumps(u, sort_keys=True)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bench-dir",
                    default=os.environ.get(
                        "INTERFLOPBENCH",
                        os.path.expanduser("~/.cache/interflopbench/examples")))
    ap.add_argument("--ikos", default=os.path.join(REPO_ROOT, "install", "bin", "ikos"))
    ap.add_argument("--clang", default=None,
                   help="three-stage mode: clang (give all three of these)")
    ap.add_argument("--ikos-pp", default=None, help="three-stage mode: ikos-pp")
    ap.add_argument("--ikos-analyzer", default=None,
                   help="three-stage mode: ikos-analyzer")
    ap.add_argument("--tier", choices=["1", "2", "all"], default="all")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--report", default=None)
    ap.add_argument("--keep-drivers", default=None)
    args = ap.parse_args()

    args.bench_dir = os.path.abspath(args.bench_dir)

    if not os.path.isdir(args.bench_dir):
        # 77 + SKIP_RETURN_CODE in CMake: an absent benchmark repo is an
        # environment gap, not a failing test. CI without network must not go
        # red over it.
        eprint("bench dir not found: %s" % args.bench_dir)
        eprint("clone it first (see this script's docstring); skipping")
        return 77

    tool = build_tool(args, REPO_ROOT)

    benches = discover(args.bench_dir)
    eprint("found %d benchmarks in %s (%s mode)"
          % (len(benches), args.bench_dir, tool.mode))

    t1 = t2 = []
    if args.tier in ("1", "all"):
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            t1 = list(ex.map(lambda b: tier1(b, tool), benches))
    if args.tier in ("2", "all"):
        t2benches = [b for b in benches if b.tier2]
        eprint("tier 2 applicable: %d / %d" % (len(t2benches), len(benches)))
        if args.keep_drivers:
            os.makedirs(args.keep_drivers, exist_ok=True)
            t2 = [tier2(b, tool,
                       keep_dir=os.path.join(args.keep_drivers, b["name"]))
                  for b in t2benches]
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
                t2 = list(ex.map(lambda b: tier2(b, tool), t2benches))

    # Write the report before printing so a reporting bug cannot lose the data.
    if args.report:
        with open(args.report, "w") as f:
            json.dump({"tier1": t1, "tier2": t2}, f, indent=1)
        eprint("report written to %s" % args.report)

    print_scorecard(t1, t2)

    unsound = sum(
        1 for b in t2 for c in b.get("cases", [])
        for p in ("nan_free", "finite")
        if c[p]["label"].startswith("UNSAT")
    )
    crashed = sum(1 for r in t1
                 if r["status"] not in ("ok", "compile-error") or r["trap"])
    return 1 if (unsound or crashed) else 0


if __name__ == "__main__":
    sys.exit(main())

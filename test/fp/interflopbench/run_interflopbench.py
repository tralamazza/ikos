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

# libm entry points IKOS leaves as opaque extern calls.
OPAQUE_MATH = {
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh",
    "tanh", "exp", "exp2", "expm1", "log1p", "fmod", "remainder", "tgamma",
    "lgamma", "cbrt", "hypot",
}


def _with_widths(names):
    """libm spells each function in float / double / long double flavours."""
    out = set(names)
    for n in names:
        out.add(n + "f")
        out.add(n + "l")
    return out


WATCHED_MATH = _with_widths(MODELED_MATH | OPAQUE_MATH)

# Base op names, used to recognise the third spelling of unmodeled math: an
# LLVM intrinsic IKOS never maps to an AR intrinsic, so it survives the whole
# pipeline as `@llvm.cos.f64`. Neither a bare-libm-name check nor the source
# regex sees it.
MATH_BASES = MODELED_MATH | OPAQUE_MATH

FLT_MAX = "3.4028234663852886e+38F"
DBL_MAX = "1.7976931348623157e+308"

# checks.status, from analyzer/test/regression/libruntest.py
STATUS = {0: "ok", 1: "warning", 2: "error", 3: "unreachable"}
# CheckerName enum order in analyzer/include/ikos/analyzer/checker/name.hpp
F2I_CHECKER = 17   # FloatToIntOverflow
DBZ_CHECKER = 1    # DivisionByZero
FPZ_CHECKER = 18   # FloatPointException

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

# The kernel lives between these pragmas in every benchmark. Anchoring on the
# pragmas rather than on line layout is what lets a signature span lines or sit
# next to attributes without the parse falling apart.
KERNEL_REGION = re.compile(r"#pragma KERNEL_START(.*?)#pragma KERNEL_STOP", re.S)
KERNEL_SIG = re.compile(
    r"([A-Za-z_][A-Za-z_0-9 \t\*]*?)\s*\bkernel\s*\(([^)]*)\)")

FLOAT_TYPES = ("float", "double", "long double")
INT_TYPES = ("int", "long", "unsigned int", "unsigned", "short", "size_t")


def _resolve_real(ty, real):
    return real if ty == "REAL" else ty


def parse_param(p):
    """Return (base_type, name, is_array).

    Array-ness sits in two places. `double U[]` trails with [], but `float *x`
    trails with the NAME and keeps the '*' inside the type. Checking only for a
    trailing '*' misses the pointer form, which is most of this suite -- that
    bug read six array kernels as scalars on the first pass.
    """
    p = " ".join(p.split())
    is_arr = False
    if p.endswith("[]"):
        is_arr, p = True, p[:-2].strip()
    m = re.match(r"^(.*?)([A-Za-z_][A-Za-z_0-9]*)$", p)
    if not m:
        return (p, "?", is_arr)
    ty, nm = m.group(1).strip(), m.group(2)
    if "*" in ty:
        is_arr = True
        ty = ty.replace("*", "").strip()
    return (ty, nm, is_arr)


def parse_kernel_shape(src, meta):
    """Work out how to call this benchmark's kernel and what to assert on.

    Returns (shape, reason). `shape` is None when the benchmark cannot be driven
    from its own ground-truth vectors, and `reason` says why, so every exclusion
    is auditable instead of silent.

    Binding is by NAME where possible. `52_div_zero` declares
    `kernel(float *x, int n)` but orders its metadata as ['n', 'x'], so a
    positional read would bind n to the array and x to the count. Positional is
    a fallback only for benchmarks whose metadata uses generic names ('x')
    against descriptive params ('celsius'), where the counts still line up.

    A pointer param is an OUT param only when `ordered_outputs` names it.
    Requiring that explicitly is what keeps `13_cancellation` out: its array `x`
    is never given values, so treating it as an out-param would have us assert on
    a value the ground truth says nothing about.
    """
    region = KERNEL_REGION.search(src)
    if not region:
        return None, "no #pragma KERNEL_START region"
    m = KERNEL_SIG.search(region.group(1))
    if not m:
        return None, "no kernel() inside KERNEL_START region"

    td = REAL_TYPEDEF.search(src)
    real = td.group(1).strip() if td else None
    raw_ret = " ".join(m.group(1).split())
    ret = _resolve_real(raw_ret, real)
    ret_is_float = ret in FLOAT_TYPES

    raw = m.group(2).strip()
    params = []
    if raw and raw != "void":
        for p in raw.split(","):
            p = " ".join(p.split())
            if p:
                params.append(parse_param(p))

    in_names = list(meta.get("ordered_inputs", []))
    out_names = list(meta.get("ordered_outputs", []))
    pnames = [p[1] for p in params]

    if not params:
        if not ret_is_float:
            return None, "return type %r is not float/double" % (raw_ret or "<none>")
        return {"ret": ret, "params": []}, None

    if set(in_names) <= set(pnames):
        binding = {n: n for n in in_names}
    elif len(in_names) == len(params):
        binding = {pnames[i]: in_names[i] for i in range(len(in_names))}
    else:
        return None, "inputs %s do not map onto params %s" % (in_names, pnames)

    bound = []
    for (ty, nm, is_arr) in params:
        ty = _resolve_real(ty, real)
        key = binding.get(nm)
        if key is not None:
            bound.append({"type": ty, "name": nm, "array": is_arr,
                         "key": key, "role": "in"})
        elif nm in out_names:
            if not is_arr:
                return None, "out-param %r is not a pointer" % nm
            bound.append({"type": ty, "name": nm, "array": True,
                         "key": None, "role": "out"})
        else:
            return None, ("param %r (%s) has no ground-truth source and is not "
                         "named in ordered_outputs" % (nm, ty))

    for p in bound:
        if p["type"] not in FLOAT_TYPES and p["type"] not in INT_TYPES:
            return None, "unsupported param type %r for %r" % (p["type"], p["name"])

    # A void kernel is still checkable when it writes its answers through
    # out-parameters named in ordered_outputs -- that is exactly what the
    # discriminant benchmarks do. Without such a param there is nothing to
    # assert on, so it stays excluded.
    if not ret_is_float:
        if ret == "void" and any(p["role"] == "out" for p in bound):
            return {"ret": "void", "params": bound}, None
        return None, ("return type %r is not float/double and the kernel has no "
                     "out-parameter to assert on" % (raw_ret or "<none>"))

    return {"ret": ret, "params": bound}, None


class Bench:
    def __init__(self, name, path):
        self.name = name
        self.path = path
        self.src = open(os.path.join(path, "bench.c")).read()
        with open(os.path.join(path, "metadata.json")) as f:
            self.meta = json.load(f)
        self.inputs = []  # list of (input_dict, errors)
        for ds in self.meta.get("datasets", []):
            ins = ds.get("inputs", [])
            if not ins:
                # Zero-arg kernels carry their error classes but no vector. Keep
                # the dataset's errors by synthesising the one vector there is.
                ins = [{}]
            for inp in ins:
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
        self.tier2, self.tier2_skip_reason = parse_kernel_shape(self.src, self.meta)



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


def run_ikos(tool, source, analyses, workdir, trace=False):
    """Run the analysis pipeline over `source` and return the result.

    Two modes, depending on what `tool` was built from:

      wrapper      --ikos <install/bin/ikos>. One command, needs the installed
                   python module.
      three-stage  --clang/--ikos-pp/--ikos-analyzer. Mirrors what
                   libruntest.py does so the harness runs straight out of the
                   build tree with no `cmake --install` step, which is what
                   ctest needs. Flag sets are imported from libruntest rather
                   than copied, so they cannot drift.

    `trace=True` adds --trace-ar-stmts so the caller can read AR-level names
    out of stdout. Costs ~20ms per benchmark.
    """
    db = os.path.join(workdir, "out.db")
    if os.path.exists(db):
        os.remove(db)

    trace_flag = ["--trace-ar-stmts"] if trace else []
    if tool.mode == "wrapper":
        cmds = [[tool.ikos, source, "-a", ",".join(analyses)] + trace_flag +
                ["-o", db]]
    else:
        bc = os.path.join(workdir, "in.bc")
        pp = os.path.join(workdir, "in.pp.bc")
        cmds = [
            tool.clang_flags + [source, "-o", bc],
            [tool.ikos_pp, "-opt=basic", bc, "-o", pp],
            [tool.ikos_analyzer, "-a=%s" % ",".join(analyses), "-d=interval",
             "-entry-points=main", "-proc=inter"] + trace_flag +
            [pp, "-o=" + db],
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


def _literal(ty, v):
    """C literal for a ground-truth value of declared type `ty`."""
    if ty in INT_TYPES:
        return str(int(float(v)))
    if ty == "float":
        return repr(float(v)) + "F"
    return repr(float(v))


def _max_for(ty):
    return FLT_MAX if ty == "float" else DBL_MAX


def gen_driver(bench, out_path):
    """Emit one case_N() per ground-truth vector.

    Each case calls the kernel with that vector and then asserts the two
    decidable properties on every value the kernel produces. A single-arg kernel
    has one target, the return value; a kernel with out-parameters has one per
    out-param, so `80_discriminant` asserts on root1 and root2 separately.
    tier2() folds the multiple lines back into one verdict per property by
    worst-case, which is the only aggregation that keeps DETECTED honest: if any
    output is refuted, a violation really was found.
    """
    shape = bench.tier2
    ctype = shape["ret"]
    lines = DRIVER_HEAD.format(bench_c=os.path.join(bench.path, "bench.c")).split("\n")
    plan = []  # (case_idx, input, errors, nan_lines, fin_lines)
    for i, (inp, errs) in enumerate(bench.inputs):
        lines.append("void case_%d(void) {" % i)
        call_args = []
        targets = []  # (var, type) to assert on
        for p in shape["params"]:
            v = "%s_%d" % (p["name"], i)
            if p["role"] == "out":
                # `0F` does not compile -- the lexer reads it as octal 0 followed
                # by a stray 'F'. Needs a decimal point.
                zero = "0.0F" if p["type"] == "float" else "0.0"
                lines.append("  %s %s = %s;" % (p["type"], v, zero))
                call_args.append("&" + v)
                targets.append((v, p["type"]))
                continue
            val = inp.get(p["key"])
            if isinstance(val, list):
                if not val:
                    raise ValueError("%s: empty array for param %r -- C has no "
                                     "zero-size arrays" % (bench.name, p["name"]))
                lines.append("  %s %s[] = { %s };"
                            % (p["type"], v,
                               ", ".join(_literal(p["type"], x) for x in val)))
                call_args.append(v)
            else:
                if p["array"]:
                    raise ValueError("%s: param %r is an array but the ground "
                                     "truth gave a scalar %r"
                                     % (bench.name, p["name"], val))
                call_args.append(_literal(p["type"], val))

        if ctype == "void":
            lines.append("  kernel(%s);" % ", ".join(call_args))
        else:
            lines.append("  %s y = kernel(%s);" % (ctype, ", ".join(call_args)))
            targets.insert(0, ("y", ctype))
        if not targets:
            raise ValueError("%s: nothing to assert on" % bench.name)

        nan_lines, fin_lines = [], []
        for (tn, tt) in targets:
            mx = _max_for(tt)
            nan_lines.append(len(lines) + 1)
            lines.append("  __ikos_assert(%s == %s);" % (tn, tn))
            fin_lines.append(len(lines) + 1)
            lines.append("  __ikos_assert(%s >= -%s && %s <= %s);"
                        % (tn, mx, tn, mx))
        lines.append("}")
        lines.append("")
        plan.append((i, inp, errs, nan_lines, fin_lines))
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
        # Same classification as tier1: a driver that will not compile because the
        # benchmark pulls in an arch-specific header is a benchmark problem, not
        # an analyzer one. 33_subnormal_func1 includes <xmmintrin.h>, which
        # hard-errors on arm64.
        low = r["stderr"].lower()
        status = ("compile-error"
                 if ("error while compiling" in low or "errors generated" in low)
                 else r["status"])
        return {"bench": bench.name, "status": status,
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

    def worst(lines):
        """Fold several output assertions into one verdict.

        Worst-case, because the property is about the case as a whole: if any
        output is refuted then a violation was found. 'missing' only wins when
        every target was silent, otherwise it would hide a real error behind a
        target the prover said nothing about.
        """
        seen = [at(l) for l in lines]
        present = [v for v in seen if v != "missing"]
        if not present:
            return "missing"
        for want in ("error", "warning", "unreachable", "ok"):
            if want in present:
                return want
        return present[0]

    for i, inp, errs, nan_lines, fin_lines in plan:
        nan_v = worst(nan_lines)
        fin_v = worst(fin_lines)
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
# Tier 3: fpz recall against div_zero ground truth
# ---------------------------------------------------------------------------

AR_FLOAT_CALL = re.compile(r"call @ar\.float\.([a-z0-9]+)\.([a-z0-9]+)")
# The dot must be in the character class: the names here are dotted
# (`llvm.cos.f64`, `ar.float.sqrt.double`), and a class without it silently
# matches none of them.
RAW_CALL = re.compile(r"call @([A-Za-z_][A-Za-z0-9_.]*)\(")


def _math_base(name):
    """Base op name: `llvm.cos.f64` -> `cos`, `cosf` -> `cos`, `sqrt` -> `sqrt`."""
    if name.startswith("llvm."):
        return name[len("llvm."):].split(".")[0]
    for suffix in ("f", "l"):
        if name.endswith(suffix) and name[:-1] in MATH_BASES:
            return name[:-1]
    return name


def trace_math(trace_text):
    """Return (ar_float_ops, unmodeled_math) from a --trace-ar-stmts dump.

    Three spellings have to be told apart, and only the first is modeled:

      @ar.float.sqrt.double   modeled AR float intrinsic
      @sqrt                   opaque libm call (glibc default on x86_64 Linux)
      @llvm.cos.f64           LLVM intrinsic IKOS never models -- also opaque

    Missing the third is how a probe can call a benchmark fully modeled while
    the analyzer understood nothing of the math in it.
    """
    ar_ops = set(m.group(1) for m in AR_FLOAT_CALL.finditer(trace_text))
    unmodeled = set()
    for m in RAW_CALL.finditer(trace_text):
        n = m.group(1)
        if n.startswith("ar."):
            continue                      # counted as modeled above
        if _math_base(n) in MATH_BASES:
            unmodeled.add(n)
    return ar_ops, unmodeled


def probe_modeled(bench, tool):
    """Classify, from the AR trace, whether this benchmark's math calls reached
    a modeled float intrinsic.

    This replaces a regex over bench.c, which could only report what the author
    wrote, not what IKOS understood. The importer rewrites a modeled math call to
    an AR intrinsic named `ar.float.<op>.<width>`, so a modeled call never
    appears under its C name. A trace still carrying `@sqrt` was not modeled.
    That asymmetry is what makes this a real check rather than a restatement of
    the source.

    The old source-level metric is what let the x86_64-Linux gap hide: Darwin
    lowers `sqrt` to `llvm.sqrt.f64` while glibc emits a plain `@sqrt` call, so
    every math-using benchmark was reported "modeled" on a platform where the
    analyzer was in fact staring at an opaque extern.
    """
    wd = tempfile.mkdtemp(prefix="ifbmodeled_")
    try:
        shutil.copy(os.path.join(bench.path, "bench.c"), wd)
        r = run_ikos(tool, "bench.c", ["uva"], wd, trace=True)
        if r["status"] != "ok":
            return {"bench": bench.name, "status": r["status"],
                    "modeled_ir": None, "opaque_ir": None, "no_math_ir": None,
                    "ar_ops": [], "raw_math": [],
                    "source_math": bench.math_used, "source_modeled": bench.modeled}
        ar_ops, unmodeled = trace_math(r.get("stdout", ""))
        has_math = bool(ar_ops or unmodeled)
        # Per-name, not per-bench: a benchmark that mixes `sqrt` and `cos` must
        # still be caught if its `sqrt` failed to map. Aggregating over
        # `all(math_used in MODELED_MATH)` hides exactly that.
        leaked = sorted(n for n in unmodeled if _math_base(n) in MODELED_MATH)
        return {"bench": bench.name, "status": "ok",
                "modeled_ir": has_math and not unmodeled,
                "opaque_ir": bool(unmodeled),
                "no_math_ir": not has_math,
                "leaked": leaked,
                "ar_ops": sorted(ar_ops), "raw_math": sorted(unmodeled),
                "source_math": bench.math_used, "source_modeled": bench.modeled}
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def tier3(bench, tool):
    """Run `fpz` and record whether it fires, against div_zero ground truth.

    The quantifiers here do not line up, and that asymmetry is the whole point of
    this tier. `metadata.json` lists errors per *dataset input*: which error
    classes the benchmark's listed inputs produce. `fpz`'s may-tier ranges over
    every value the divisor could take. So a benchmark whose listed inputs never
    hit a zero divisor can still legitimately warn, and 27 of 93 do. That is not
    unsoundness and is not counted against the checker.

    The direction that must never regress is the other one: a benchmark that
    really can divide by zero must not come back silent. Silence on a real
    div_zero is a lost crash on a target that traps.

    Note the definite tier contributes nothing on this suite -- all 93 benchmarks
    take nondet input, so every reachable zero divisor is a "may", never a
    "definitely". The may-tier is what carries recall here, which is why it is
    kept despite the noise.
    """
    wd = tempfile.mkdtemp(prefix="ifb3_")
    try:
        shutil.copy(os.path.join(bench.path, "bench.c"), wd)
        r = run_ikos(tool, "bench.c", ["fpz"], wd)
        c = f2i_counts(r["db"], FPZ_CHECKER)
        return {"bench": bench.name, "status": r["status"], "fpz": c,
                "fired": bool(c.get("error") or c.get("warning")),
                "truth_div_zero": "div_zero" in bench.all_errors}
    finally:
        shutil.rmtree(wd, ignore_errors=True)


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


def print_scorecard(tier1_res, tier2_res, tier3_res=None, probes=None):
    probes = probes or []
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
    t2_status = {}
    for b in tier2_res:
        t2_status[b["status"]] = t2_status.get(b["status"], 0) + 1
    print("  driver status: %s" % json.dumps(t2_status, sort_keys=True))
    for b in tier2_res:
        if b["status"] not in ("ok", "compile-error"):
            print("  !! %-28s %s" % (b["bench"], b["status"]))
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

    # Split precision by whether the math actually reached a modeled AR float
    # intrinsic, as observed in the trace. The old split used a regex over the C
    # source, which reported author intent rather than analyzer capability.
    ir = {p["bench"]: p for p in probes}

    def in_bucket(b, kind):
        return bool(ir.get(b["bench"], {}).get(kind))

    for prop in ("nan_free", "finite"):
        m = tally([b for b in tier2_res if in_bucket(b, "modeled_ir")], prop)
        u = tally([b for b in tier2_res if in_bucket(b, "opaque_ir")], prop)
        n = tally([b for b in tier2_res if in_bucket(b, "no_math_ir")], prop)
        print("  %s  [IR-verified] modeled   = %s" % (prop, json.dumps(m, sort_keys=True)))
        print("  %s  [IR-verified] opaque    = %s" % (prop, json.dumps(u, sort_keys=True)))
        print("  %s  [IR-verified] no-libm   = %s" % (prop, json.dumps(n, sort_keys=True)))

    if probes:
        print()
        print("  modelling probe (AR trace, not source regex):")
        ok = [p for p in probes if p["status"] == "ok"]
        bad = [p for p in probes if p["status"] != "ok"]
        print("    probed=%d  fully-modeled=%d  opaque=%d  no-libm=%d  probe-failed=%d"
              % (len(ok),
                 sum(1 for p in ok if p["modeled_ir"]),
                 sum(1 for p in ok if p["opaque_ir"]),
                 sum(1 for p in ok if p["no_math_ir"]),
                 len(bad)))
        # The interesting set: a call whose name IKOS is supposed to model came
        # back opaque. This is exactly the class of platform/frontend gap the old
        # source regex could not see -- it is how the whole FP model sat inert on
        # x86_64 Linux while every benchmark still read "modeled".
        lied = [p for p in ok if p["leaked"]]
        if lied:
            print("    names IKOS should model but the call stayed OPAQUE: %d" % len(lied))
            for p in lied[:20]:
                print("      %-32s leaked=%s ar_ops=%s"
                      % (p["bench"], p["leaked"], p["ar_ops"]))
            if len(lied) > 20:
                print("      ... %d more" % (len(lied) - 20))
        else:
            print("    every should-be-modeled call reached an AR float intrinsic.")
        if bad:
            print("    probe failures: %s"
                  % [(p["bench"], p["status"]) for p in bad[:10]])

    if not tier3_res:
        return

    print()
    print("=" * 72)
    print("TIER 3 -- fpz recall against div_zero ground truth (%d benchmarks)"
          % len(tier3_res))
    print("=" * 72)
    dz = [r for r in tier3_res if r["truth_div_zero"]]
    hit = [r for r in dz if r["fired"]]
    miss = [r for r in dz if not r["fired"]]
    print("  div_zero ground truth : %d" % len(dz))
    print("  fpz fired           : %d" % len(hit))
    print("  fpz SILENT (BUG)    : %d %s" % (len(miss), [r["bench"] for r in miss]))
    extra = [r for r in tier3_res if not r["truth_div_zero"] and r["fired"]]
    print("  fired w/o div_zero truth: %d" % len(extra))
    print("     -> expected, not counted against the checker. The dataset inputs")
    print("        never hit a zero divisor, but the divisor is unconstrained so")
    print("        a zero is reachable on some input. This is the may-tier's")
    print("        noise floor; it is also the only tier with any recall here,")
    print("        since every benchmark input is nondet.")
    for r in hit:
        print("     %-32s %s" % (r["bench"], r["fpz"]))


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
    ap.add_argument("--tier", choices=["1", "2", "3", "all"], default="all")
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

    t1 = t2 = t3 = []
    if args.tier in ("1", "all"):
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            t1 = list(ex.map(lambda b: tier1(b, tool), benches))
    if args.tier in ("2", "all"):
        t2benches = [b for b in benches if b.tier2]
        eprint("tier 2 applicable: %d / %d" % (len(t2benches), len(benches)))
        if args.keep_drivers:
            os.makedirs(args.keep_drivers, exist_ok=True)
            t2 = [tier2(b, tool,
                       keep_dir=os.path.join(args.keep_drivers, b.name))
                  for b in t2benches]
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
                t2 = list(ex.map(lambda b: tier2(b, tool), t2benches))

    if args.tier in ("3", "all"):
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            t3 = list(ex.map(lambda b: tier3(b, tool), benches))

    # The modelling probe runs over every benchmark regardless of tier: whether
    # the frontend turned a math call into a modeled AR intrinsic is a property
    # of the importer, not of any one tier.
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        probes = list(ex.map(lambda b: probe_modeled(b, tool), benches))

    # Write the report before printing so a reporting bug cannot lose the data.
    if args.report:
        with open(args.report, "w") as f:
            json.dump({"tier1": t1, "tier2": t2, "tier3": t3, "probes": probes},
                      f, indent=1)
        eprint("report written to %s" % args.report)

    print_scorecard(t1, t2, t3, probes)

    unsound = sum(
        1 for b in t2 for c in b.get("cases", [])
        for p in ("nan_free", "finite")
        if c[p]["label"].startswith("UNSAT")
    )
    crashed = sum(1 for r in t1
                 if r["status"] not in ("ok", "compile-error") or r["trap"])
    # Tier 2 now drives 91 benchmarks through generated drivers, so a crash in
    # there has to reach the exit code too. Before this, tier2 failures were only
    # visible in the report file -- a driver that blew up looked like a pass.
    t2_crashed = [b for b in t2 if b["status"] not in ("ok", "compile-error")]
    # A benchmark with real div_zero ground truth that fpz went silent on.
    fpz_missed = sum(1 for r in t3 if r["truth_div_zero"] and not r["fired"])
    if fpz_missed:
        for r in t3:
            if r["truth_div_zero"] and not r["fired"]:
                eprint("  fpz SILENT on div_zero benchmark: %s" % r["bench"])
    # A call whose name IKOS is supposed to model, but which the AR trace shows
    # stayed an opaque extern. Zero on macOS and on Linux with the libm mapping;
    # non-zero means that mapping regressed. This is the guard that was missing
    # when the FP model was silently inert on x86_64 Linux.
    modelling_leaks = [(p["bench"], p["leaked"]) for p in probes
                       if p.get("leaked")]
    if modelling_leaks:
        for b, leaked in modelling_leaks[:10]:
            eprint("  MODELLING REGRESSION: %s should be modeled but stayed "
                   "opaque: %s" % (b, leaked))
    if t2_crashed:
        for b in t2_crashed:
            eprint("  tier2 FAILED (%s): %s" % (b["status"], b["bench"]))
    return 1 if (unsound or crashed or fpz_missed or t2_crashed
                or modelling_leaks) else 0


if __name__ == "__main__":
    sys.exit(main())

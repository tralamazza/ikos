#!/usr/bin/env python3
"""Render the InterflopBench JSON report as a compact CI summary.

Why this is a separate script and not part of run_interflopbench.py:
$GITHUB_STEP_SUMMARY is a CI concern and has no business inside a test script
that also runs locally. The harness produces data; this turns it into a page.

Why it exists at all: `ctest --output-on-failure` hides a passing test's
stdout, so the harness scorecard -- the only place the modelling coverage and
precision tallies appear -- was discarded on every green run. The exit-code
guards still fire on a regression, but nothing showed the numbers, so slow
shrinkage was invisible.

Usage:
    ci_summary.py <report.json>

Writes markdown to stdout, and appends to $GITHUB_STEP_SUMMARY when set.

Exits 0 when the report is absent. An absent report means the harness skipped
(no benchmark checkout), and a skip must not be turned into a failure -- that
is the same trap as caching a half-written benchmark tree.
"""

import json
import os
import sys

UNSAT = ("UNSAT_FALSE_PROVEN", "UNSAT_TRUE_REFUTED")


def tally(cases, prop):
    out = {}
    for c in cases:
        lab = c.get(prop, {}).get("label", "?")
        out[lab] = out.get(lab, 0) + 1
    return out


def fmt(d):
    return " ".join("%s=%s" % (k, d[k]) for k in sorted(d))


def build(md, report):
    meta = report.get("meta", {})
    t1 = report.get("tier1", [])
    t2 = report.get("tier2", [])
    t3 = report.get("tier3", [])
    probes = report.get("probes", [])

    target = meta.get("target_triple", "unknown")
    md.append("### InterflopBench \u2014 FP validation")
    md.append("")
    md.append("`%s` on `%s` (%s mode)"
             % (target, meta.get("platform", "?"), meta.get("mode", "?")))
    md.append("")
    md.append("| Tier | Metric | Value |")
    md.append("|---|---|---|")

    # --- Tier 1: robustness -------------------------------------------------
    ok = sum(1 for r in t1 if r.get("status") == "ok")
    cerr = sum(1 for r in t1 if r.get("status") == "compile-error")
    other = [r for r in t1 if r.get("status") not in ("ok", "compile-error")]
    traps = sum(1 for r in t1 if r.get("trap"))
    md.append("| 1 robustness | clean | %d |" % ok)
    md.append("| | compile-error | %d |" % cerr)
    md.append("| | other status | %d%s |"
             % (len(other), "" if not other
                else " " + ", ".join("%s:%s" % (r["bench"], r["status"])
                                    for r in other[:3])))
    md.append("| | traps | %d |" % traps)

    # --- Tier 2: coverage + soundness --------------------------------------
    driven = sum(1 for b in t2 if b.get("status") == "ok")
    cases = [c for b in t2 for c in b.get("cases", [])]
    unsound = [(b, c, p) for b in t2 for c in b.get("cases", [])
               for p in ("nan_free", "finite")
               if c.get(p, {}).get("label") in UNSAT]
    md.append("| 2 coverage | benchmarks driven | %d |" % driven)
    md.append("| | input vectors | %d |" % len(cases))
    md.append("| | **unsound verdicts** | **%d**%s |"
             % (len(unsound),
                "" if not unsound else " \u2190 BUG"))

    # --- Probe: is the math actually modelled ------------------------------
    pok = [p for p in probes if p.get("status") == "ok"]
    md.append("| probe | fully-modelled | %d |"
             % sum(1 for p in pok if p.get("modeled_ir")))
    md.append("| | opaque | %d |" % sum(1 for p in pok if p.get("opaque_ir")))
    md.append("| | no libm | %d |" % sum(1 for p in pok if p.get("no_math_ir")))
    leaked = [p for p in pok if p.get("leaked")]
    md.append("| | **modelling leaks** | **%d**%s |"
             % (len(leaked), "" if not leaked else " \u2190 BUG"))
    if leaked:
        for p in leaked[:5]:
            md.append("| | | `%s` %s |" % (p["bench"], p["leaked"]))

    # --- Tier 3: fpz recall ------------------------------------------------
    dz = [r for r in t3 if r.get("truth_div_zero")]
    fired = [r for r in dz if r.get("fired")]
    silent = [r for r in dz if not r.get("fired")]
    md.append("| 3 fpz | div_zero ground truth | %d |" % len(dz))
    md.append("| | fired | %d |" % len(fired))
    md.append("| | **silent** | **%d**%s |"
             % (len(silent), "" if not silent else " \u2190 BUG"))
    if silent:
        md.append("| | | %s |" % ", ".join(r["bench"] for r in silent[:5]))

    # --- Precision tallies -------------------------------------------------
    md.append("")
    md.append("Precision by IR-verified modelling bucket:")
    md.append("")
    bucket = {}
    for p in probes:
        if p.get("modeled_ir"):
            bucket[p["bench"]] = "modelled"
        elif p.get("opaque_ir"):
            bucket[p["bench"]] = "opaque"
        elif p.get("no_math_ir"):
            bucket[p["bench"]] = "no-libm"
        else:
            bucket[p["bench"]] = "unprobed"

    for prop in ("nan_free", "finite"):
        md.append("| Bucket | %s |" % prop)
        md.append("|---|---|")
        for want in ("modelled", "opaque", "no-libm", "unprobed"):
            cs = [c for b in t2
                  if bucket.get(b.get("bench")) == want
                  for c in b.get("cases", [])]
            if not cs:
                continue
            md.append("| %s | %s |" % (want, fmt(tally(cs, prop))))

    return md


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    md = []

    if not path or not os.path.exists(path):
        # A skip, not a failure. The harness exits 77 before writing anything
        # when the benchmark checkout is absent.
        print("InterflopBench: no report at %s -- harness skipped "
              "(benchmark checkout absent?)" % path)
        md.append("### InterflopBench \u2014 skipped")
        md.append("")
        md.append("No report file. The harness skips when the benchmark "
                  "checkout is absent, so there is nothing to report.")
    else:
        try:
            with open(path) as f:
                report = json.load(f)
        except (ValueError, OSError) as e:
            print("InterflopBench: could not read report %s: %s" % (path, e))
            return 0
        build(md, report)

    out = "\n".join(md) + "\n"
    sys.stdout.write(out)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        try:
            with open(summary, "a") as f:
                f.write(out)
        except OSError as e:
            # Cosmetic only; never fail a build over a summary write.
            print("(could not append to GITHUB_STEP_SUMMARY: %s)" % e)
    return 0


if __name__ == "__main__":
    sys.exit(main())

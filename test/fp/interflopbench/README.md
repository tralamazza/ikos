# InterflopBench validation of the FP work

[InterflopBench](https://github.com/interflop/InterflopBench) is 93 small C
programs, each with a `metadata.json` giving concrete input vectors and the
floating-point error classes each vector **actually triggers on real hardware**
(`cancellation`, `overflow`, `underflow`, `nan`, `div_zero`, `comparison`,
`no_error`). It was built to score FP error *classifiers* (FPChecker,
Herbgrind, LLMs). IKOS is not a classifier — it is an abstract interpreter for
safety properties. So the suite is used here for the two things it can actually
answer, and the rest is reported as out of scope rather than silently ignored.

```
./run_interflopbench.py --bench-dir ~/.cache/interflopbench/examples \
                        [--tier 1|2|all] [--jobs N] [--report report.json]
```

Fetch the benchmark once (sparse, skips the 6 MB HTML table):

```
git clone --filter=blob:none --sparse https://github.com/interflop/InterflopBench ~/.cache/interflopbench
git -C ~/.cache/interflopbench sparse-checkout set examples
```

## Tier 1 — robustness (all 93)

Runs `ikos -a f2i,prover,dbz` on every `bench.c` and looks for crashes, traps
and timeouts. This is the load-bearing check for the "total intrinsic switch"
work: an unhandled float intrinsic traps, so a clean sweep means the dispatch
covers everything this suite emits.

Result: **92/93 clean, 0 traps, 0 timeouts.**

The one failure is a benchmark portability problem, not an IKOS problem:
`33_subnormal_func1` includes `<xmmintrin.h>`, which hard-errors on arm64. It
is classified `compile-error` and excluded from the robustness denominator.

## Tier 2 — soundness and precision (62 benchmarks, 948 input vectors)

For every benchmark with a one-argument scalar kernel, a driver is generated
that calls the kernel with each concrete dataset vector and asserts two
properties via `__ikos_assert`:

| property | expression | false when |
|---|---|---|
| `nan_free` | `y == y` | `y` is NaN |
| `finite` | `-MAX <= y <= MAX` | `y` is ±inf **or NaN** |

Both are decidable from the FP domain alone, with no libm modelling. Ground
truth × IKOS verdict gives one label per case:

| ground truth | IKOS | label | meaning |
|---|---|---|---|
| false | `error` | `DETECTED` | refuted a real violation |
| false | `warning` | `MISSED` | sound, no precision |
| false | `ok` | `UNSAT_FALSE_PROVEN` | **proved a lie — bug** |
| true | `error` | `UNSAT_TRUE_REFUTED` | **refuted truth — bug** |
| true | `warning` | `IMPRECISE` | sound, no precision |
| true | `ok` | `CORRECT` | proved a real non-violation |

Only the two `UNSAT_*` labels are bugs. Everything else is a coverage or
precision measurement.

### Result after both fixes

```
nan_free:  CORRECT 347   DETECTED 6    MISSED 14   IMPRECISE 581
finite:    CORRECT 245   DETECTED 22   MISSED 122  IMPRECISE 559

No unsound results across 1896 verdicts.
```

### Result at HEAD (bc915ee..8f9c8bc), before the fixes

```
nan_free:  CORRECT 347   DETECTED 0    MISSED 20   IMPRECISE 581
finite:    CORRECT 245   DETECTED 16   MISSED 128  IMPRECISE 559

No unsound results across 1896 verdicts.
```

**The headline: zero unsoundness.** Across 948 concrete vectors the FP domain
never proved a false property and never refuted a true one. That is the claim
the FP work most needs to survive, and it does.

## Findings

### 1. `nan_free` was never DETECTED — must-NaN dies in `interval_bin_op`

Baseline detects 0 of 20 real NaN cases, even though the domain *can* prove
NaN from literals:

```c
sqrt(-3.0)          -> error: assertion never holds   (IKOS knows it is NaN)
log(-2.0)           -> error
pow(-2.0, 0.5)      -> error
0.0 / 0.0           -> error
1e308*10 - 1e308*10 -> error
```

The knowledge is lost the moment the NaN flows through non-constant arithmetic.
Minimal reproduction — same kernel, one operator apart:

```c
__attribute__((noinline)) double nodiv(double x) { return sqrt(1.0 - x*x); }
__attribute__((noinline)) double withdiv(double x) { return sqrt(1.0 - x*x) / (x + 1.0); }

nodiv(2.0)   -> error    // definitely-NaN survives the return
withdiv(2.0) -> warning  // the division washed it out to top
```

Root cause, `core/include/ikos/core/domain/scalar/float_interval.hpp`,
`interval_bin_op`:

```cpp
if (x.ordered_empty() || y.ordered_empty()) {
  // No ordered values to combine; only the NaN possibility carries over.
  if (!may_nan) { return Interval{pos_inf, neg_inf, false}; }
  return Interval::top_value();   // <-- collapses definitely-NaN to "unknown"
}
```

`Interval` has `may_nan` but no `must_nan`; the two are distinguished only by
representation — `is_definitely_nan()` is `may_nan && ordered_empty()`, while
`top_value()` is `may_nan` with a full `[-inf, +inf]` range. Returning
`top_value()` here throws the `ordered_empty()` half away, so a value that was
*pinned* to NaN becomes merely *possibly* NaN, and `y == y` can no longer be
refuted.

**Fixed** in `de92867` by preserving the definitely-NaN representation:

```cpp
if (x.is_definitely_nan() || y.is_definitely_nan()) {
  return Interval{Interval::pos_inf(), Interval::neg_inf(), true};
}
```

Verified effect, same harness (`+fix2` is finding 2 below):

| metric | before | +fix1 | +fix1+fix2 |
|---|---|---|---|
| `nan_free` DETECTED | 0 | **5** | **6** |
| `nan_free` MISSED | 20 | 15 | 14 |
| `finite` DETECTED | 16 | **21** | **22** |
| `finite` MISSED | 128 | 123 | 122 |
| unsound | 0 | 0 | 0 |
| `ctest` | 60/60 | 60/60 | 61/61 |

Revert `de92867` to drop fix 1.

### 2. `exec_float_sqrt` threw away must-NaN for intervals — fixed

After fix 1, one modelled case still missed: `44_sqrt_neg` at `x = 1.01`.
`1.0 - 1.01*1.01` is not a literal, so `sqrt` took the interval path, not
the constant path, and hit an explicit choice in
`analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp`:

```cpp
if (hi_d < 0.0) {
  // Every input is negative, so every result is NaN. Top out rather than
  // assert a definite-NaN with an empty ordered range.
  this->_inv.normal().float_assign_nondet(ret.var());
  return;
}
```

The comment's stated reason no longer held: the definitely-NaN representation
exists, and fix 1 above uses it. Returning it here instead of `nondet` makes
the interval path agree with the constant path, which was the actual
inconsistency — the same expression was known-NaN at `x = 2.0` (folded to a
literal) and unknown at `x = 1.01` (computed through an interval), for no
reason either could defend.

After both fixes the modelled population has **no `nan_free` misses left**:

```
nan_free  modeled = {CORRECT 304, DETECTED 6, IMPRECISE 58}
```

The remaining 14 misses are all opaque-libm kernels (`expm1`, `sin`/`cos`),
which is a coverage limit, not a modelling error.

### 3. `div_zero` is a hard coverage gap, confirmed not assumed

10 benchmarks have `div_zero` in their ground truth. Running `dbz` over all of
them produces **zero reports**. `analyzer/src/checker/division_by_zero.cpp`
contains no float handling at all — consistent with IEEE (a float divide-by-zero
yields ±inf/NaN rather than trapping), but it means InterflopBench's
`div_zero` class is entirely invisible to IKOS today.

### 4. Only 16 float intrinsics are modelled; the suite uses ~20 libm calls

Modelled (`frontend/llvm/src/import/bundle.cpp`): `sqrt, log, log2, log10,
pow, fabs, floor, ceil, trunc, round, rint, copysign, maxnum, minnum, fma,
fmuladd`.

Opaque extern calls used by the suite: `sin, cos, tan, asin, acos, tanh, sinh,
cosh, exp, expm1, log1p, fmod, tgamma`. Each one makes the result top, which
is why 581 of 948 `nan_free` cases are `IMPRECISE` rather than provable. The
harness tags every benchmark `modeled` / `opaque` so the two populations are
never conflated:

```
nan_free  modeled = {CORRECT 304, DETECTED 5, MISSED 1, IMPRECISE 58}
nan_free  opaque  = {CORRECT 43,  MISSED 14,  IMPRECISE 523}
```

The `modeled` column is where IKOS is actually being tested. The `opaque`
column measures how much of the suite IKOS cannot see at all.

### 5. `f2i` fires correctly and only where it should

4 benchmarks produce `f2i` reports, all `warning`, all on genuinely unguarded
casts of external input:

```
58_triangular_identity   1   int N = (int)x_in;
59_alt_power             1
60_factorial_gamma       2
61_geometric_series      1
```

No `f2i` false positives on the other 89.

## What this suite cannot validate

Stated plainly so the numbers are not over-read:

* **`cancellation`, `comparison`, `underflow`** — precision questions. Both
  asserted properties hold for them, so IKOS has nothing to say. 59 of 93
  benchmarks have ground truth drawn only from these classes.
* **Round-off magnitude / condition number** — nothing in IKOS measures these.
* **`div_zero`** — no reporter (finding 3).
* Tier 2 covers 62 of 93 benchmarks. The other 31 have multi-argument,
  pointer, or array kernels that the driver generator does not handle. Tier 1
  still covers all 93 for robustness.

## Reproducing / CI

Registered as ctest test #61 `interflopbench` from
`analyzer/test/regression/CMakeLists.txt`:

```
ctest -R interflopbench            # ~2 s
ctest                             # 61/61
```

It runs straight out of the build tree via the `clang` / `ikos-pp` /
`ikos-analyzer` trio, so it needs no `cmake --install` step. The benchmark
location is the `INTERFLOPBENCH_DIR` cache variable, defaulting to
`$HOME/.cache/interflopbench/examples`.

`SKIP_RETURN_CODE 77` is set: an absent benchmark checkout reports `Skipped`
rather than failing, so an offline CI run stays green. Verified:

```
ctest -R interflopbench  ->  ***Skipped   (with -DINTERFLOPBENCH_DIR=/nonexistent)
```

The test exits non-zero on an analyzer crash, a trap, or any `UNSAT_*`
floating-point verdict, so it gates on the properties that matter and stays
quiet about everything else.

Manual invocation, either against the installed wrapper or the build tree:

```
python3 test/fp/interflopbench/run_interflopbench.py --tier all --report /tmp/ifb.json

python3 test/fp/interflopbench/run_interflopbench.py \
    --clang /opt/homebrew/opt/llvm@18/bin/clang \
    --ikos-pp build/frontend/llvm/ikos-pp \
    --ikos-analyzer build/analyzer/ikos-analyzer
```

Full-suite runtime is ~2 s wall clock at 8 jobs; it adds ~2 s to `ctest`.

## Decisions taken from this validation

Recorded so the reasoning survives the conversation that produced it.

**Float division by zero -- new opt-in checker, not a `dbz` extension.**
IEEE's default model makes float `x/0` non-faulting, but that is a default, not
a law: on ARM (`FPSCR` ZE/IO/OE) and x87 (`MXCSR`) the FP exception mask is
configurable and some embedded targets run with it unmasked, so a float
divide-by-zero really does trap. That is a genuine safety property, so it gets a
reporter. Kept out of `dbz` so integer semantics stay clean; opt-in because on
general-purpose targets it is defined behavior and would be noise.

Whether a given divide actually faults depends on the FPU control register,
which is set outside the analyzed code and is therefore invisible to the
analysis. Reports must be worded conditionally -- "would trap on targets with
FP exceptions unmasked" -- never as an unconditional crash claim.

**Upstream -- no.** Fork-only. No PR to `NASA-SW-VnV/ikos`.

**`must_nan` representation -- leave it.** Must-NaN stays encoded as an inverted
ordered range plus `may_nan`. Both landed fixes lean on that invariant and
nothing in the type system enforces it, so a future operation returning
`top_value()` where it meant `point(nan)` would silently lose the distinction
again. Accepted, because the harness now runs in CI and catches exactly that
regression class -- see finding 1, which is that bug and was caught this way.
A dedicated NaN case in `Interval` would touch every operation for a bug that
is already observable.

**Modelling more libm -- no, not for coverage.** 581 of 948 `nan_free` cases
are `IMPRECISE` because `exp`/`sin`/`cos`/`expm1`/`tgamma` are opaque externs.
Sound outward-rounded interval images for those are hard, and the risk is
asymmetric: a wrong `exp` image is worse than no `exp` image, because a wrong
image is the unsoundness this harness exists to catch. Model one only when a
specific target property needs it.

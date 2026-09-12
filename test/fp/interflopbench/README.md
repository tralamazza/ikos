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

## Tier 2 — soundness and precision (91 benchmarks, 1133 input vectors)

For every benchmark whose kernel can be driven from its own ground-truth
vectors, a driver is generated that calls the kernel with each concrete
dataset vector and asserts two properties via `__ikos_assert`:

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

### 3. `div_zero` was a hard coverage gap — now closed by `fpz`

10 benchmarks have `div_zero` in their ground truth. Running `dbz` over all of
them produces **zero reports**. `analyzer/src/checker/division_by_zero.cpp`
contains no float handling at all — consistent with IEEE (a float divide-by-zero
yields ±inf/NaN rather than trapping), but it meant InterflopBench's
`div_zero` class was entirely invisible to IKOS.

Closed by the `fpz` checker (Tier 3). Measured over all 93 benchmarks:

| | count |
|---|---|
| div_zero ground truth | 10 |
| `fpz` fired | **10** |
| `fpz` silent (would be a bug) | **0** |
| fired without div_zero ground truth | 27 |

The 27 extra firings are not false positives. `metadata.json` records errors per
*dataset input*; `fpz`'s may-tier ranges over every value the divisor could
take. Spot-checked three and each is a genuinely unconstrained divisor: `a / b`
(`79_division`), `sqrt(temp) / (x + 1.0)` where `x` can be −1 (`44_sqrt_neg`),
`a/(2*b)` (`03_rump`). Different quantifier, not unsoundness.

The reason the may-tier is load-bearing rather than merely noisy: **the definite
tier contributes zero across all 93 benchmarks.** Every InterflopBench kernel
takes nondet input, so a reachable zero divisor is always a "may", never a
"definitely". Dropping the may-tier for tidiness would have taken recall to 0/10.

### 4. Only 16 float intrinsics are modelled; the suite uses ~20 libm calls

Modelled (`frontend/llvm/src/import/bundle.cpp`): `sqrt, log, log2, log10,
pow, fabs, floor, ceil, trunc, round, rint, copysign, maxnum, minnum, fma,
fmuladd`.

Opaque: `sin, cos, tan, asin, acos, tanh, sinh, cosh, exp, expm1, log1p,
fmod, tgamma`. Each one makes the result top.

#### A math call has three spellings, and only one is modelled

Read `--trace-ar-stmts` and every math call turns out to be one of:

| trace | meaning |
|---|---|
| `@ar.float.sqrt.double` | modelled AR float intrinsic |
| `@sqrt` | opaque libm call |
| `@llvm.cos.f64` | LLVM intrinsic IKOS never maps -- **also opaque** |

The third is the trap. It looks like an intrinsic, so it reads as "handled", but
IKOS has no transfer function for it and the result is top.

#### The platform split this hid

Whether a math call even *arrives* as an intrinsic is target-dependent:

| target | `log(x)` compiles to |
|---|---|
| `arm64-apple-darwin` | `llvm.log.f64` |
| `x86_64-apple-darwin` | `llvm.log.f64` |
| `x86_64-unknown-linux-gnu` | `@log` (plain glibc call) |
| `x86_64-unknown-linux-gnu -fno-math-errno` | `llvm.log.f64` |

x86_64 Linux defaults to `-fmath-errno`, so glibc's `math.h` lowers to ordinary
library calls. IKOS keys off `ar::Intrinsic::Float*`, which only arrives via the
LLVM intrinsic, so on Linux the whole FP model was inert -- every modelled
function was an opaque extern. Fixed by mapping libm names onto the AR intrinsics
in the importer (`translate_libm_intrinsic`).

#### Why the old `modeled` tag could not see any of this

It was a regex over `bench.c`:

```python
self.math_used = sorted(m for m in set(MATH_CALL.findall(self.src)) ...)
self.modeled = all(m in MODELED_MATH for m in self.math_used)
```

That reports what the author wrote, not what IKOS understood. Every `sqrt` read
as "modelled" on a platform where the analyzer was looking at `@log`. It also
counted a benchmark with *no* math as modelled, since `all([])` is true.

The probe now reads the AR trace instead, and splits into three buckets so the
vacuous case is visible rather than folded into "modelled":

```
probed=92  fully-modeled=27  opaque=40  no-libm=25  probe-failed=1
every should-be-modeled call reached an AR float intrinsic.

nan_free  modeled = {CORRECT 298, DETECTED 8, IMPRECISE 32, MISSED 3}
nan_free  opaque  = {IMPRECISE 514, MISSED 37}
nan_free  no-libm = {CORRECT 121, IMPRECISE 111, MISSED 9}
```

The split is now sharp in a way it was not before: **every** `CORRECT` and
`DETECTED` result lives in the modelled bucket; the opaque bucket has none.
That is an IR-grounded demonstration that the modelling, not luck, is what
produces the precision.

#### The guard

Any call whose name IKOS is supposed to model but which the trace shows stayed
opaque fails the suite:

```
MODELLING REGRESSION: 11_cosine_func1 should be modeled but stayed opaque: ['llvm.cos.f64']
```

This is per-name, not per-bench, so a benchmark mixing `sqrt` and `cos` still
gets caught on its `sqrt`. Verified load-bearing by adding `cos` to
`MODELED_MATH`: the guard fired on every cosine benchmark, catching
`llvm.cos.f32` as well as `llvm.cos.f64`, and the run exited 1.

The DB cannot be used for this. It stores LLVM-level names only (`llvm.sqrt.f64`
on Darwin, `sqrt` on Linux) and `strings` finds no `ar.float` anywhere in it, so
it can report the platform split but not whether the call was modelled.

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

### 6. Tier 2 covered only 62 of 93 — generalised to 91

The original driver generator handled exactly one shape: `kernel(REAL x)`
returning `REAL`. Everything else was skipped, so 31 benchmarks got Tier 1
robustness only and **never had a single per-input soundness verdict**. That is
the dangerous kind of coverage gap: the suite looked broadly green while a third
of it was untested for the property that matters.

The 31 break down as:

| shape | count | now covered? |
|---|---|---|
| 2-arg kernel | 17 | yes |
| 3-arg kernel | 7 | yes |
| `int`-only input, float return | 2 | yes |
| zero-arg, float return | 1 | yes |
| `void` return, out-params | 3 | yes |
| `const char*` return | 1 | no — property does not apply |
| array param with no ground-truth values | 1 | no — would fabricate inputs |

Four things the generalisation had to get right, each found by looking at real
benchmarks rather than assuming:

**Bind by name, not position.** `52_div_zero` declares `kernel(float *x, int n)`
but orders its metadata `['n', 'x']`. A positional read binds `n` to the array
and `x` to the count — silently wrong, and it would have produced garbage
verdicts rather than an error. Positional is kept only as a fallback for
benchmarks whose metadata uses generic names (`x`) against descriptive params
(`celsius`, `angle_factor`, `principal/rate/time`), where the counts still line
up.

**Pointer array-ness hides in the type.** `double U[]` trails with `[]`, but
`float *x` trails with the *name* and keeps the `*` inside the type. Checking
only for a trailing `*` read six array kernels as scalars. Caught by
categorising all 93 before writing the generator.

**Out-params need `ordered_outputs` to name them.** The discriminant benchmarks
return `void` and write answers through `float *root1, *root2`. Requiring the
name to appear in `ordered_outputs` is what keeps `13_cancellation` out: its
array `x` is never given values, so treating it as an out-param would have us
assert on a value the ground truth says nothing about. Fabricating inputs and
trusting the labels anyway is how a validator starts lying.

**`0F` does not compile.** The out-param initialiser emitted `float r = 0F;`,
which the lexer reads as octal `0` followed by a stray `F`. Needs `0.0F`.
Caught by bulk-compiling all 91 generated drivers before running any analysis.

Verification that the new paths are load-bearing rather than vacuously passing:

- **Zero label changes** across all 62 previously-covered benchmarks, checked
  per-input-vector. The generalisation is behaviour-preserving on existing
  coverage.
- The void/out-param path was proven in **both directions** on a synthetic
  `void kernel(double x, double *out1, double *out2)` writing `sqrt(x)`:
  `x=4.0` → `CORRECT` (proved), `x=-4.0` → `DETECTED` (refuted `out1==out1`).
- Tier 2 failures now reach the exit code. They previously did not — a driver
  that blew up was visible only in the report file and the run still exited 0.
  Verified by forcing a bad status: exit 1.

Newly-covered benchmarks produce real verdicts, including 4 new `DETECTED`
cases (`07_heron_func1` ×2, `25_rump` ×2). Still zero unsoundness.

Remaining uncovered: `13_cancellation` (no ground-truth values for its array)
and `62_possiblenan` (returns `const char*`; NaN and finiteness are not
properties of a string).

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

### CI wiring

Both `.github/workflows/build-linux.yml` and `build-macos.yml` now run the
suite. Three steps are load-bearing and each was verified locally, because each
fails in a way that is easy to miss:

**`make build-core-tests` is required.** The core unit-test binaries hang off a
custom `build-core-tests` target and are **not** part of the default `all`.
Running `ctest` after a plain `make` reports 37 tests as `***Not Run` and fails
the job. This is the trap that would have made the first CI attempt red for a
reason unrelated to the code.

**The InterflopBench checkout must be fetched.** Tier 2 and Tier 3 need it, and
the test SKIPs (exit 77) when it is absent. Without the fetch step the job goes
green while never checking a single per-input floating-point verdict — green by
absence. The fetch is cached (`actions/cache`, keyed `interflopbench-examples-*`)
and `continue-on-error: true`, so upstream downtime cannot redden us; the skip
then shows up explicitly as `***Skipped` in the ctest output.

**A half-written checkout must never reach the cache.** If the clone succeeds but
`sparse-checkout` fails, the bench directory exists without `examples/`. The
test would skip, and the cache would persist that useless tree so every later
run skips too, silently, forever. The step checks for `examples/` and wipes the
directory with a `::warning::` if it is missing, so the next run retries.

Verified locally, Debug build (asserts on, matching CI):

```
ctest                                   -> 61/61 pass, exit 0
bench checkout hidden                   -> interflopbench ***Skipped, exit 0
sparse-checkout failure (simulated)     -> warning + dir wiped
```

**Debug vs Release.** CI builds `-DCMAKE_BUILD_TYPE=Debug`, which leaves
`ikos_assert` / `ikos_unreachable` live (`CMAKE_CXX_FLAGS_DEBUG=-g`, no
`-DNDEBUG`). Both configurations were run: Debug 61/61 and Release 61/61. The
assert-enabled path is therefore known-good rather than assumed.

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

**`fpz` scope -- divide-by-zero plus invalid-operation, not overflow.**
Implemented as `FloatPointExceptionChecker` (`-a fpz`, `CheckerName::
FloatPointException` = 18, `CheckKind::FloatPointException` = 41). Covers:

* `fdiv` with a definitely-zero divisor → ZE error; possibly-zero → ZE warning
* `0/0` → invalid-operation error, *not* divide-by-zero. Reporting the weaker
  class would point the reader at the wrong exception bit.
* `frem` by a definitely/possibly zero divisor → invalid (IEEE remainder is
  undefined there, which is IO not ZE)
* `+ - *` whose interval result is pinned to NaN → invalid (`inf - inf`,
  `inf * 0`, NaN propagating from an upstream pinned operand)
* `sqrt`/`log`/`log2`/`log10` of a definitely-negative operand → invalid.
  NaN counts as negative here: `sqrt(NaN)` raises IO just as `sqrt(-1)` does,
  so an interval pinned to NaN is a definite domain error, not an unknown one.

Overflow to infinity is **not** reported. Measured on this suite it is provable
in only 22 of 129 real cases, so an overflow checker would sit silent exactly
when it matters and let the output read as "clean". A checker that is wrong by
omission is worse than no checker.

`log(0)` is **not** reported. C raises `FE_DIVBYZERO` for it as a library
convention, but no hardware FP exception accompanies it -- the hardware returns
`-inf` and carries on. Reporting it would claim a trap that cannot happen.

**Enum append discipline.** `CheckKind` and `CheckerName` are persisted as raw
integers and mirrored positionally in `enums.py`, so both new entries are
appended last. Verified end-to-end rather than assumed: a DB written by the C++
side reads back as checker 18 / kind 41 through the Python enums, and the
`ikos-report` message generator resolves them.

**A test that cannot fail is not a test.** Both new guards were verified
load-bearing by deliberately breaking them: flipping a `test-fpz.c` line
expectation makes `analysis-float` fail, and pointing the harness's
`FPZ_CHECKER` id at a wrong value makes Tier 3 exit 1 naming each silent
benchmark. Worth recording because the first version of the fpz test file passed
while testing nothing -- every float result was unused, so clang's DCE deleted
all the arithmetic and the checker never saw a single `fdiv`. The tests use a
`volatile` sink to keep the operations alive.

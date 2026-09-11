# Floating-point validation harness

## Why this directory exists

The `bc915ee` commit message describes 40 differential validators run against
the FP implementation, ending in `fp_unsound = 0`. **None of those validators
were ever committed.** They lived in `.auto/tools/`, which `.gitignore` excludes,
and only six `.pyc` files survived:

```
.auto/tools/__pycache__/fp_diff_check_nan_identity.cpython-314.pyc
.auto/tools/__pycache__/fp_diff_check_storage.cpython-314.pyc
.auto/tools/__pycache__/fp_fuzz_casttree.cpython-314.pyc
.auto/tools/__pycache__/fp_fuzz_cells.cpython-314.pyc
.auto/tools/__pycache__/fp_fuzz_fma_cast.cpython-314.pyc
.auto/tools/__pycache__/fp_fuzz_nd.cpython-314.pyc
```

So the branch's headline validation claim was not reproducible from the repo.
This directory makes what can be recovered reproducible, and records what cannot.

## What is here

| File | Status | Covers |
|---|---|---|
| `fp_diff_check_nan_identity.py` | **reconstructed, passing** | `x P x` for all 14 IEEE predicates, in NaN and ordered contexts, via hand-written LLVM IR |
| `recovered_docstrings.txt` | **recovered verbatim** | the docstrings of all six original validators |

Run it:

```
./install/bin/ikos --version >/dev/null            # build must be installed
python3 test/fp/fp_diff_check_nan_identity.py     # needs llvm-as (llvm@18)
```

Expected output:

```
predicates tested  : 14 x 2 contexts = 28
must-prove         : 14
must-not-prove     : 14
SAME-VAR NAN IDENTITY PASS
```

Guard liveness was checked: flipping `oeq` to claim it is provable in the NaN
context fails with `oeq nan context: expected provable but reported DEFINITE
UNSAFE`. The validator is not vacuous.

## A finding: the original NaN-identity validator could not have passed

Reconstructing it surfaced a contradiction between the validator and its own
data. The recovered bytecode checked, for every case:

```python
if unsafe > 0:
    problems.append("reported DEFINITE UNSAFE, but the identity is decidable here")
```

with no reference to whether that case was supposed to be provable.

But all 14 of the table's `must-not-prove` entries are **genuinely false
assertions** in the context they are placed in:

- `x ogt x` is always false, ordered or not
- `x oeq x` is false when `x` is NaN
- `x one x` is always false

Placing `__ikos_assert(x ogt x)` in a context where it cannot hold means the
program really does violate an assertion. IKOS reports `assertion never holds`,
which is the **correct** verdict. The validator counted 14 correct verdicts as
failures.

The table itself is sound: cross-checked against real IEEE-754 semantics in
Python, all 28 entries agree. IKOS agrees with the table on all 28. Only the
validator's pass rule disagreed.

The rule is corrected here to flag definite-unsafe only when the identity was
supposed to be provable, which is the only actual contradiction. The deviation
is marked inline in the source.

**Implication.** At least one of the validators behind the `fp_unsound = 0`
claim was internally inconsistent. That does not make the FP implementation
wrong — on this property the implementation is right and the checker-of-the-
checker was wrong — but it does mean the claim itself should not be trusted as
evidence. Treat the FP work as validated only by what is in this directory and
by `analyzer/test/regression/fp/`, not by the commit message.

## What is NOT reconstructed, and why

The other five validators are fuzzers (`fp_fuzz_*`) and a storage-shape
differential (`fp_diff_check_storage`). They were not rebuilt because:

1. **No Python 3.14 decompiler exists.** `uncompyle6`, `decompyle3` and
   `pycdc` all stop well short of 3.14 bytecode. Only `dis` works, which gives
   control flow but not readable source.
2. **A guessed reconstruction would be worse than none.** A fuzzer whose oracle
   is subtly wrong produces confident false assurance. The NaN-identity case
   above is exactly how that looks. Reconstructing 5 fuzzers from bytecode and
   then trusting them would be manufacturing evidence.
3. **The highest-value coverage now lives in ctest.** `analyzer/test/regression/
   fp/` holds the crash regression, the IEEE rounding tests and the `f2i` tier
   tests, wired into `ctest` as `analysis-float`, each with guard liveness
   verified. Those run on every build; these do not.

Their docstrings are preserved in `recovered_docstrings.txt`, which records the
trap classes they targeted and the reasoning, so the knowledge is not lost even
where the code is.

## Recommended next step if you want the fuzzers back

Rewrite them forward from the docstrings rather than backward from the bytecode,
and treat each as unproven until it has been shown to fail against a deliberately
broken build. Start with `fp_fuzz_nd` — its docstring describes the simplest
oracle (random expression, random guard, compare IKOS's verdict against Python's
own evaluation), so a wrong reconstruction is easiest to spot.

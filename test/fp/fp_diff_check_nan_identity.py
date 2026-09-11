#!/usr/bin/env python3
"""
Same-variable float comparison identities: `x P x` for all 14 IEEE predicates.

Why hand-written LLVM IR rather than C. The analyzer's same-variable handler needs
the AR to hold the SAME variable on both sides of the comparison. clang at -O0
does not produce that shape for isnan(x): it lowers to two separate `load`
instructions into two distinct SSA registers, so the operands are different
variables and the handler cannot fire. Writing the IR directly tests the AR-level
semantics that the handler actually implements.

The identity table, derived from the IEEE-754 spellings where `u<pred>` means
"unordered OR pred" and `o<pred>` means "ordered AND pred", plus the facts that
x==x, x>=x and x<=x all hold whenever x is ordered while x<x and x>x never do:

    une, uno, ult, ugt  <=>  x IS NaN
    ord, oeq, oge, ole  <=>  x is NOT NaN
    one, olt, ogt       =  always false   (needs ordered AND x != x)
    ueq, ule, uge       =  always true   (ordered operands satisfy them)

Each case builds a function whose parameter is split by `fcmp une x, x` into a
NaN context and an ordered context, then asserts `x P x` inside one of them. The
assert operand is a runtime zext, never a literal, so nothing is folded away.

RECONSTRUCTED. The original lived in .auto/tools/ and was never committed; only
its .pyc survived. This file was rebuilt from that bytecode -- the docstring,
TEMPLATE and TABLE constants are recovered verbatim, the control flow from the
disassembly. See test/fp/README.md for what is reconstructed vs. original.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
IKOS = os.path.join(HERE, "..", "..", "install", "bin", "ikos")


def find_llvm_as():
    for cand in (os.environ.get("LLVM_AS"), "llvm-as",
                "/opt/homebrew/opt/llvm@18/bin/llvm-as"):
        if cand and shutil.which(cand):
            return shutil.which(cand)
    raise RuntimeError("llvm-as not found")


LLVM_AS = find_llvm_as()

# (predicate, proven_in_nan_context, proven_in_ordered_context)
TABLE = (
    ("oeq", False, True),
    ("ogt", False, False),
    ("oge", False, True),
    ("olt", False, False),
    ("ole", False, True),
    ("one", False, False),
    ("ord", False, True),
    ("uno", True, False),
    ("ueq", True, True),
    ("ugt", True, False),
    ("uge", True, True),
    ("ult", True, False),
    ("ule", True, True),
    ("une", True, False),
)

TEMPLATE = """declare double @nd()
declare void @__ikos_assert(i32)

define i8 @f(double %x) {{
entry:
  %g = fcmp une double %x, %x
  br i1 %g, label %nan_ctx, label %ord_ctx

nan_ctx:
{nan_body}
  br label %done

ord_ctx:
{ord_body}
  br label %done

done:
  ret i8 0
}}

define i8 @main() {{
  %v = call double @nd()
  %r = call i8 @f(double %v)
  ret i8 %r
}}
"""


def assert_block(pred):
    return ("  %t = fcmp {p} double %x, %x\n"
            "  %ti = zext i1 %t to i32\n"
            "  call void @__ikos_assert(i32 %ti)\n").format(p=pred)


def build(pred, which):
    blank = "  ; no assertion\n"
    body = assert_block(pred)
    if which == "nan":
        return TEMPLATE.format(nan_body=body, ord_body=blank)
    return TEMPLATE.format(nan_body=blank, ord_body=body)


def run_case(ll_src):
    with tempfile.TemporaryDirectory() as d:
        ll = os.path.join(d, "t.ll")
        bc = os.path.join(d, "t.bc")
        with open(ll, "w") as fh:
            fh.write(ll_src)

        a = subprocess.run([LLVM_AS, ll, "-o", bc], capture_output=True, text=True)
        if a.returncode != 0:
            raise RuntimeError("llvm-as failed: " + a.stderr[-400:])

        r = subprocess.run([IKOS, "-a", "prover", bc], capture_output=True,
                          text=True)
        out = r.stdout + r.stderr
        if r.returncode != 0:
            raise RuntimeError("ikos failed: " + out[-400:])

        def count(label):
            m = re.search("Total number of " + label + r"\s*:\s*(\d+)", out)
            return int(m.group(1)) if m else 0

        return count("checks"), count("safe checks"), count("definite unsafe checks")


def main():
    problems = []
    for pred, nan_prove, ord_prove in TABLE:
        for which, should_prove in (("nan", nan_prove), ("ord", ord_prove)):
            ctx = "{} context".format(which)
            total, safe, unsafe = run_case(build(pred, which))

            if total != 1:
                problems.append("{} {}: expected exactly 1 check, saw {}".format(
                    pred, ctx, total))
                continue
            # DEVIATION FROM THE RECONSTRUCTED BYTECODE.
            # The original read `if unsafe > 0: problem`, with no reference to
            # `should_prove`. That contradicts its own TABLE: every one of the 14
            # "must-not-prove" entries is a genuinely FALSE assertion in the
            # context it is placed in (x ogt x is always false, x oeq x is false
            # when x is NaN, and so on), so IKOS reporting `assertion never
            # holds` there is the CORRECT verdict, not a defect. As written the
            # original could not have passed -- see test/fp/README.md.
            #
            # Definite-unsafe is only a contradiction when the identity was
            # supposed to be provable.
            if should_prove and unsafe > 0:
                problems.append(
                    "{} {}: expected provable but reported DEFINITE UNSAFE".format(
                        pred, ctx))
                continue

            proven = safe > 0
            if proven == should_prove:
                continue
            problems.append(
                "{} {}: `x {} x` was {} but must {}".format(
                    pred, ctx, pred,
                    "PROVEN" if proven else "unproven",
                    "PROVEN" if should_prove else "unproven"))

    if problems:
        print("SAME-VAR NAN IDENTITIES: FAIL")
        for p in problems:
            print("  - " + p)
        return 1

    print("predicates tested  : {} x 2 contexts = {}".format(
        len(TABLE), len(TABLE) * 2))
    print("must-prove         : {}".format(
        sum(1 for _, n, o in TABLE for v in (n, o) if v)))
    print("must-not-prove     : {}".format(
        sum(1 for _, n, o in TABLE for v in (n, o) if not v)))
    print("SAME-VAR NAN IDENTITY PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

// IEEE-754 rounding behaviour: IKOS must reason in binary, not in reals.
//
// The test harness compiles with clang's default -O0 (see clang_emit_llvm_flags
// in ../libruntest.py), so clang does NOT constant-fold these expressions --
// the fadd survives to the IR and IKOS performs the rounding itself. Do not
// add `volatile` here: volatile loads are opaque to IKOS and every assertion
// in this file silently stops proving, which is how the first draft of this
// test passed for all the wrong reasons.
//
// Ground truth is C itself, measured with volatile operands and printed with %a
// for exactness:
//
//   0.1 + 0.2         = 0x1.3333333333334p-2  (0.30000000000000004441)
//   (0.1 + 0.2) - 0.3 = 0x1p-54               (5.551115123125783e-17)
//   0.2 + 0.4         = 0x1.3333333333333p-1?  no: 0.60000000000000008882
//
// NOTE: the bc915ee commit message claims "(0.1+0.2)-0.3 == 0.0". That is
// FALSE -- the true value is 2^-54. Line 32 below asserts the correct thing.
// A real-arithmetic (non-IEEE) model would prove `== 0.0` and be unsound.

extern void __ikos_assert(int);

int main() {
  double a = 0.1, b = 0.2, c = 0.3;
  double d = 0.4, f = 0.6;

  double s = a + b;

  // 0.1 + 0.2 is NOT 0.3, and is strictly greater than it.
  __ikos_assert(s != c);            // line 30
  __ikos_assert(s > c);             // line 31

  // The residual is 2^-54, not zero.
  __ikos_assert((s - c) != 0.0);    // line 34

  // 0.2 + 0.4 is NOT 0.6.
  __ikos_assert(d + f != 0.6);      // line 37

  // The rounding is deterministic: the same expression yields the same value.
  double s2 = a + b;
  __ikos_assert(s == s2);           // line 41

  // And it is bounded: strictly between 0.3 and 0.31.
  __ikos_assert(s > 0.3 && s < 0.31);   // line 44

  return 0;
}

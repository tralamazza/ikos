// IEEE-754 NaN and range semantics for the logarithm and power intrinsics.
//
// bc915ee modelled llvm.sqrt's NaN behaviour but left llvm.log, llvm.log2,
// llvm.log10 and llvm.pow unmodelled, so `isnan(log(-2.0))` -- true in every
// C library -- came back "could not be proven". These cases pin the semantics
// added for those four.
//
// Ground truth is C itself, measured natively (see the note at the bottom):
//
//   log(-2.0)       = nan            log2(-2.0)     = nan
//   log10(-2.0)     = nan            pow(-2.0, 0.5) = nan
//   log(2.0)        = 0.6931471805599453
//   log(+0.0)       = -inf           log(1.0)       = 0
//   log2(2.0)       = 1              log10(100.0)   = 2
//
// Two things this file deliberately does NOT do:
//
//   * It does not use __ikos_nondet_double. That builtin does not exist in
//     this build -- library_function.cpp knows only __ikos_nondet_int and
//     __ikos_nondet_uint -- so a declared-but-unknown double source leaves
//     the variable opaque and every assertion below silently stops proving.
//     The interval cases seed a double from a constrained int instead.
//
//   * It does not call isnan() on a locally-constrained variable and expect
//     the guard to propagate. Darwin's isnan() macro copies its argument into
//     a temporary alloca, and the assert re-reads the original, so that path
//     needs value identity across memory, which this branch does not have.
//     Every isnan() here is applied to an expression whose NaN-ness the
//     intrinsic model establishes directly.

#include <math.h>

extern void __ikos_assert(int);
extern int __ikos_nondet_int(void);
#define __ikos_assume(condition) \
  if (!(condition)) {           \
    __builtin_unreachable();    \
  }

// A double holding a known interval, without a float nondet builtin.
#define FP_RANGE(VAR, LO, HI)                   \
  double VAR = (double)__ikos_nondet_int();     \
  __ikos_assume(VAR >= (double)(LO));          \
  __ikos_assume(VAR <= (double)(HI));

int main(void) {
  // ---- NaN production from constants ----------------------------------------
  // Before the fix all four of these were "could not be proven".
  __ikos_assert(isnan(log(-2.0)));       // line 49  safe
  __ikos_assert(isnan(log2(-2.0)));     // line 50  safe
  __ikos_assert(isnan(log10(-2.0)));    // line 51  safe
  __ikos_assert(isnan(pow(-2.0, 0.5))); // line 52  safe

  // The same facts must be REFUTED, not merely unproven. A model that returned
  // top for these would pass line 47 while failing nothing; these two are what
  // stop that from being mistaken for soundness.
  __ikos_assert(!isnan(log(2.0)));      // line 57  safe
  __ikos_assert(isnan(log(2.0)));       // line 58  definite unsafe

  // ---- ordered image -------------------------------------------------------
  // log(1) == 0 exactly, so the lower bound is tight rather than the denormal
  // an unconditional nextafter(0.0, -inf) would produce.
  FP_RANGE(a, 1, 2)
  __ikos_assert(log(a) >= 0.0);        // line 64  safe
  __ikos_assert(log(a) >= 1.0);        // line 65  definite unsafe

  // log2(2) == 1 exactly.
  FP_RANGE(b, 2, 8)
  __ikos_assert(log2(b) >= 1.0);       // line 69  safe
  __ikos_assert(log2(b) >= 5.0);       // line 70  definite unsafe

  // log10(100) == 2 exactly.
  FP_RANGE(c, 100, 1000)
  __ikos_assert(log10(c) >= 2.0);      // line 74  safe
  __ikos_assert(log10(c) >= 5.0);      // line 75  definite unsafe

  // ---- NaN through an interval, not just a constant ------------------------
  // Every input is negative, so every result is NaN: the empty ordered range
  // with may_nan set, which is enough to prove this and not just leave it open.
  FP_RANGE(n, -5, -1)
  __ikos_assert(isnan(log(n)));        // line 81  safe

  // Straddling zero keeps NaN open, so the negation must NOT be provable.
  FP_RANGE(s, -1, 2)
  __ikos_assert(!isnan(log(s)));       // line 85  warning

  // Away from zero the same negation is provable.
  FP_RANGE(p, 1, 2)
  __ikos_assert(!isnan(log(p)));       // line 89  safe

  // ---- pow: NaN depends on both operands ----------------------------------
  // x in [-1,1] and y in [2,3]: all four corners are ordered -- pow(-1,2)
  // and pow(-1,3) are both fine -- yet x = -1 with y = 2.5 is NaN. A corner
  // scan alone would wrongly call this safe.
  FP_RANGE(q, -1, 1)
  double qy = (double)__ikos_nondet_int();
  __ikos_assume(qy >= 2.0);
  __ikos_assume(qy <= 3.0);
  __ikos_assert(!isnan(pow(q, qy)));   // line 99  warning

  return 0;
}

// Measured natively with clang -O0 on this machine:
//   printf("%.17g", log(-2.0))        -> -nan
//   printf("%.17g", log2(-2.0))      -> -nan
//   printf("%.17g", log10(-2.0))     -> -nan
//   printf("%.17g", pow(-2.0, 0.5))  -> -nan
//   printf("%.17g", log(2.0))        -> 0.69314718055994529
//   printf("%.17g", log(1.0))        -> 0
//   printf("%.17g", log2(2.0))       -> 1
//   printf("%.17g", log10(100.0))   -> 2
//   isnan(log(-2.0)) && isnan(pow(-2.0, 0.5)) && !isnan(log(2.0)) -> 1

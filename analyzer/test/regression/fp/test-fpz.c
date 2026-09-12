#include <math.h>

extern double __ikos_nondet_double(void);

volatile double sink;

/*
 * Regression tests for the `fpz` floating-point exception checker.
 *
 * Every case is written with a non-constant operand because clang constant-folds
 * arithmetic on literals -- `1.0/0.0`, `sqrt(-4.0)`, `log(-2.0)` all become
 * constants at -O0 and there is no runtime operation left to raise an exception.
 * The guards pin the value down for the analyzer without giving clang anything to
 * fold.
 */

/* Divisor pinned to +0.0: definite divide-by-zero. */
__attribute__((noinline)) void ze_definite(void) {
  double a = __ikos_nondet_double();
  if (a == 0.0) {
    sink = 5.0 / a;
  }
}

/* 0/0 belongs to the invalid-operation class, not divide-by-zero. */
__attribute__((noinline)) void zero_over_zero(void) {
  double a = __ikos_nondet_double();
  double b = __ikos_nondet_double();
  if (a == 0.0 && b == 0.0) {
    sink = b / a;
  }
}

/* Divisor unconstrained: possible, not certain. */
__attribute__((noinline)) void ze_may(void) {
  double c = __ikos_nondet_double();
  sink = 1.0 / c;
}

/* Guarded away from zero: must stay silent. */
__attribute__((noinline)) void ze_safe(void) {
  double a = __ikos_nondet_double();
  if (a >= 1.0 && a <= 2.0) {
    sink = 10.0 / a;
  }
}

/* sqrt / log of a definitely-negative operand: invalid. */
__attribute__((noinline)) void invalid_domain(void) {
  double a = __ikos_nondet_double();
  double b = __ikos_nondet_double();
  if (a < 0.0) {
    sink = sqrt(a);
  }
  if (b < -1.0) {
    sink = log(b);
    sink = log2(b);
    sink = log10(b);
  }
}

/* Guarded to a valid domain: must stay silent. */
__attribute__((noinline)) void domain_safe(void) {
  double a = __ikos_nondet_double();
  if (a >= 1.0 && a <= 2.0) {
    sink = sqrt(a);
    sink = log(a);
  }
}

/*
 * inf - inf and inf * 0. Whether these fire depends on the domain being able to
 * pin the operand to +inf; the guard below is what tries. If the analyzer widens
 * to a range instead of a point the result is merely "may be NaN", which the
 * checker deliberately does not report.
 */
__attribute__((noinline)) void invalid_arith(void) {
  double a = __ikos_nondet_double();
  if (a >= 1.0e308) {
    sink = a - a;
    sink = a * 0.0;
  }
}

int main(void) {
  ze_definite();
  zero_over_zero();
  ze_may();
  ze_safe();
  invalid_domain();
  domain_safe();
  invalid_arith();
  return 0;
}

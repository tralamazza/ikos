// Regression test for the FP-intrinsic SIGTRAP.
//
// bc915ee added the 12 Float* intrinsic IDs to ar::Intrinsic::ID and handled
// them in the numerical engine, but forgot them in the exhaustive switches over
// intrinsic IDs in the memory-related checkers. Each of those switches ends in
// ikos_unreachable("unreachable"), so calling any math.h function trapped with
// SIGTRAP (exit 133) under the default checker set.
//
// The point of this file is that the analyzer TERMINATES. It is run under boa,
// nullity, sound and watch -- the four checkers that trapped -- and expects no
// memory error. Any future intrinsic added to ar::Intrinsic::ID without a
// matching case here will crash the analyzer and fail this test, which is the
// class of bug that let the original regression through.

extern double __ikos_nondet_double(void);
extern float __ikos_nondet_float(void);

#include <math.h>

int main() {
  double x = __ikos_nondet_double();
  float y = __ikos_nondet_float();

  // Sink: keep every result live so no intrinsic is constant-folded away. A
  // folded-away intrinsic tests nothing, which is the trap this whole file is
  // built to avoid.
  double sink = 0.0;

  sink += sqrt(x);
  sink += fabs(x);
  sink += fma(x, 2.0, 1.0);
  sink += fmax(x, 1.0);
  sink += fmin(x, -1.0);
  sink += floor(x);
  sink += ceil(x);
  sink += trunc(x);
  sink += round(x);
  sink += rint(x);
  sink += copysign(x, -1.0);
  sink += fmod(x, 3.0);

  // binary32 variants of the same intrinsics.
  sink += sqrtf(y);
  sink += fabsf(y);
  sink += fmaf(y, 2.0f, 1.0f);
  sink += fmaxf(y, 1.0f);
  sink += fminf(y, -1.0f);
  sink += floorf(y);
  sink += ceilf(y);
  sink += truncf(y);
  sink += roundf(y);
  sink += rintf(y);
  sink += copysignf(y, -1.0f);

  return sink == 424242.0;
}

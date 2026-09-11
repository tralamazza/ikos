// f2i: float-to-int conversion overflow tiers.
//
// Three tiers, all verified against C's own behaviour:
//   error    -- every value in the range overflows the target integer
//   warning  -- part of the range overflows, or the range is unknown
//   silent   -- the conversion is proven in range (no check emitted)
//
// The unguarded case (line 23) is the regression this file exists for. It used
// to emit NOTHING: the checker gated its interval analysis on `!may_nan`, and a
// nondeterministic double always has may_nan == true, so the most dangerous
// cast in real code -- an unvalidated external double -- was silently accepted,
// while strictly-less-informative one-sided bounds such as `x <= 1e300` already
// warned. Top being quieter than a partial bound is backwards.

extern void __ikos_assert(int);
extern double __ikos_nondet_double(void);

int main() {
  double x = __ikos_nondet_double();

  // Tier: warning. Nothing is known about x, so it might overflow.
  // Previously silent.
  int a = (int)x;                     // line 23
  __ikos_assert(a != 424242);         // line 24

  // Tier: error. Every value >= 1e10 overflows a 32-bit int.
  if (x >= 1e10) {
    int b = (int)x;                     // line 28
    __ikos_assert(b != 424242);         // line 29
  }

  // Tier: warning. One-sided bound -- unbounded below still overflows.
  if (x <= 1e300) {
    int c = (int)x;                     // line 34
    __ikos_assert(c != 424242);         // line 35
  }

  // Tier: silent. Proven in range, so no check is emitted at all.
  if (x >= 0.0 && x <= 100.0) {
    int d = (int)x;
    __ikos_assert(d != 424242);         // line 41
  }

  // Tier: silent. Narrowed cast inside a guard that bounds the product.
  if (x >= 0.0 && x <= 1.0) {
    int e = (int)(x * 255.0);
    __ikos_assert(e != 424242);         // line 47
  }

  return 0;
}

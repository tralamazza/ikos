/*******************************************************************************
 *
 * \file
 * \brief Floating-point to integer conversion overflow checker
 *
 * Author: ikos FP support
 *
 * Notices:
 *
 * Copyright (c) 2011-2019 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT LIMITED
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS,
 * ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL BE
 * ERROR FREE, OR ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED, WILL CONFORM TO
 * THE SUBJECT SOFTWARE. THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
 * ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY RESULTS,
 * RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER APPLICATIONS
 * RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT AGENCY
 * DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY SOFTWARE,
 * IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL
 * AS ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS
 * IN ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH
 * USE, INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM,
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS
 * WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.  RECIPIENT'S
 * SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE, UNILATERAL
 * TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#include <cmath>

#include <ikos/core/domain/scalar/float_interval.hpp>

#include <ikos/analyzer/checker/float_to_int_overflow.hpp>
#include <ikos/analyzer/util/log.hpp>

namespace ikos {
namespace analyzer {

FloatToIntOverflowChecker::FloatToIntOverflowChecker(Context& ctx)
    : Checker(ctx) {}

CheckerName FloatToIntOverflowChecker::name() const {
  return CheckerName::FloatToIntOverflow;
}

const char* FloatToIntOverflowChecker::description() const {
  return "Float to int conversion overflow checker";
}

void FloatToIntOverflowChecker::check(ar::Statement* stmt,
                                      const value::AbstractDomain& inv,
                                      CallContext* call_context) {
  auto un = dyn_cast< ar::UnaryOperation >(stmt);
  if (un == nullptr) {
    return;
  }

  bool is_signed = false;
  if (un->op() == ar::UnaryOperation::FPToSI) {
    is_signed = true;
  } else if (un->op() != ar::UnaryOperation::FPToUI) {
    return;
  }

  std::optional< CheckResult > check = this->check_conversion(un, inv,
                                                             is_signed);
  if (!check.has_value()) {
    // Nothing known about the operand; do not emit a check.
    return;
  }

  this->display_invariant(check->result, stmt, inv);
  this->_checks.insert(check->kind,
                       CheckerName::FloatToIntOverflow,
                       check->result,
                       stmt,
                       call_context,
                       std::array< ar::Value*, 1 >{{un->operand()}},
                       check->info);
}

namespace {

/// \brief Truncate a bound for INTERVAL reasoning.
///
/// `FloatingPoint::trunc_toward_zero()` returns NaN for an infinite bound. That
/// is the right answer for a single value -- infinity has no integer image -- but
/// the wrong one for a bound: an interval reaching +inf means the truncated
/// values are UNBOUNDED ABOVE, not unknown.
///
/// Left as NaN it silently defeats the straddle test below, because libc++'s
/// `min(a, b)` is `(b < a) ? b : a`: with a NaN first argument the comparison
/// `b < NaN` is false, so it returns the NaN. `max` behaves the same way. Both
/// endpoints then become NaN, every comparison against them is false, and a cast
/// that genuinely overflows on one side reports nothing at all.
///
/// Measured before fixing: `x <= 2^31` (interval [-inf, 2^31], may_nan false)
/// emitted no report, though x could be -1e300 whose `(int)` truncation
/// overflows. Keeping the infinity makes that a correct `might overflow`.
double trunc_bound(FloatingPoint const& f) {
  if (f.is_inf()) {
    return f.signbit() ? -std::numeric_limits<double>::infinity()
                      : std::numeric_limits<double>::infinity();
  }
  // Still NaN if f is NaN; the caller excludes may_nan intervals, so that
  // cannot happen here, and staying silent is the conservative outcome anyway.
  return f.trunc_toward_zero();
}

} // end anonymous namespace

bool FloatToIntOverflowChecker::fits(double t, uint32_t bits, bool is_signed) {
  if (std::isnan(t)) {
    // NaN has no integer image at all.
    return false;
  }

  // Bounds are exact powers of two, and the upper bound is exclusive: a signed
  // `bits`-bit integer reaches 2^(bits-1) - 1, an unsigned one 2^bits - 1.
  // A double can never equal 2^k - 1 for large k, so an exclusive comparison
  // never admits an out-of-range value.
  double hi = is_signed ? std::ldexp(1.0, static_cast< int >(bits) - 1)
                        : std::ldexp(1.0, static_cast< int >(bits));
  double lo = is_signed ? -hi : 0.0;
  return (t >= lo && t < hi);
}

std::optional< FloatToIntOverflowChecker::CheckResult >
FloatToIntOverflowChecker::check_conversion(ar::UnaryOperation* stmt,
                                            const value::AbstractDomain& inv,
                                            bool is_signed) {
  if (inv.is_normal_flow_bottom()) {
    // Statement unreachable
    if (auto msg = this->display_check(Result::Unreachable, stmt)) {
      *msg << "\n";
    }
    return CheckResult{CheckKind::Unreachable, Result::Unreachable, {}};
  }

  auto type = dyn_cast< ar::IntegerType >(stmt->result()->type());
  if (type == nullptr) {
    // Not converting to an integer we can bound; nothing to check.
    return {};
  }
  auto bits = static_cast< uint32_t >(type->bit_width());

  const ScalarLit& lit = this->_lit_factory.get_scalar(stmt->operand());

  // A known constant decides the question outright.
  const FloatingPoint* cst = nullptr;
  if (lit.is_floating_point()) {
    cst = &lit.floating_point();
  } else if (lit.is_floating_point_var()) {
    cst = inv.normal().float_get_cst(lit.var());
  }

  if (cst != nullptr) {
    if (cst->is_nan()) {
      // NaN is undefined to convert, but it is not an overflow, and reporting it
      // here would conflate two different defects. Stay quiet.
      return {};
    }

    double t = cst->trunc_toward_zero();
    if (!this->fits(t, bits, is_signed)) {
      if (auto msg = this->display_check(Result::Error, stmt)) {
        *msg << "float to int overflow: value is outside the target range\n";
      }
      return CheckResult{CheckKind::FloatToIntOverflow, Result::Error, {}};
    }

    if (auto msg = this->display_check(Result::Ok, stmt)) {
      *msg << "float to int conversion is in range\n";
    }
    return CheckResult{CheckKind::FloatToIntOverflow, Result::Ok, {}};
  }

  // No constant: fall back to the interval. Truncation toward zero is monotonic,
  // so the truncated values of the whole interval lie between the truncated
  // endpoints. Only when both endpoints are beyond the same edge is every value
  // guaranteed to overflow.
  if (lit.is_floating_point_var()) {
    const core::floating_point::Interval* iv =
        inv.normal().float_get_interval(lit.var());
    if (iv != nullptr && !iv->is_empty() && !iv->may_nan) {
      double tlo = trunc_bound(iv->lo);
      double thi = trunc_bound(iv->hi);

      double hi = is_signed ? std::ldexp(1.0, static_cast< int >(bits) - 1)
                            : std::ldexp(1.0, static_cast< int >(bits));
      double lo = is_signed ? -hi : 0.0;

      if (tlo >= hi || thi < lo) {
        if (auto msg = this->display_check(Result::Error, stmt)) {
          *msg << "float to int overflow: every value in the interval overflows\n";
        }
        return CheckResult{CheckKind::FloatToIntOverflow, Result::Error, {}};
      }

      // The interval straddles a range edge: some values convert cleanly and some
      // do not. That is not an overflow we can assert, but it is a cast we can
      // prove is risky, and `Result::Warning` says exactly that.
      double trunc_lo = std::min(tlo, thi);
      double trunc_hi = std::max(tlo, thi);
      if (trunc_lo < lo || trunc_hi >= hi) {
        if (auto msg = this->display_check(Result::Warning, stmt)) {
          *msg << "float to int overflow: part of the interval overflows\n";
        }
        return CheckResult{CheckKind::FloatToIntOverflow, Result::Warning, {}};
      }
    }
  }

  // Operand value unknown: cannot prove overflow either way.
  return {};
}

} // end namespace analyzer
} // end namespace ikos

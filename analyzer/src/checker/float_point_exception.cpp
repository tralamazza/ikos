/*******************************************************************************
 *
 * \file
 * \brief Floating-point exception checker
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
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS, ANY
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR
 * FREEDOM FROM INFRINGEMENT, ANY ERROR FREE, OR ANY OTHER WARRANTY OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED OR STATUTORY. THIS AGREEMENT DOES NOT, IN ANY MANNER,
 * CONSTITUTE AN ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY
 * RESULTS, RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER
 * APPLICATIONS RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT
 * AGENCY DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY
 * SOFTWARE, IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL AS
 * ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS IN
 * ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH USE,
 * INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM, RECIPIENT'S
 * USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD HARMLESS THE
 * UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL AS ANY
 * PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.  RECIPIENT'S SOLE REMEDY
 * FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE, UNILATERAL TERMINATION OF THIS
 * AGREEMENT.
 *
 ******************************************************************************/

#include <string>

#include <ikos/ar/semantic/intrinsic.hpp>
#include <ikos/core/domain/scalar/float_interval.hpp>

#include <ikos/analyzer/checker/float_point_exception.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>

namespace ikos {
namespace analyzer {

namespace fp = core::floating_point;

namespace {

/// \brief Suffix appended to every finding.
///
/// Whether any of these actually faults depends on the FPU exception mask, which
/// is configured at startup outside the analyzed code and is invisible here. A
/// bare "division by zero" would claim more than the analysis knows.
const char* const kZECondition =
    "; would trap on a target with the floating-point divide-by-zero exception "
    "unmasked (ARM FPSCR.DZ, x86 MXCSR.ZE)";

const char* const kIOCondition =
    "; would trap on a target with the floating-point invalid-operation "
    "exception unmasked (ARM FPSCR.ID, x86 MXCSR.IE)";

/// \brief Map the AR float operator onto the interval arithmetic operator char
bool interval_op_of(ar::BinaryOperation::Operator op, char& out) {
  switch (op) {
    case ar::BinaryOperation::FAdd:
      out = '+';
      return true;
    case ar::BinaryOperation::FSub:
      out = '-';
      return true;
    case ar::BinaryOperation::FMul:
      out = '*';
      return true;
    case ar::BinaryOperation::FDiv:
      out = '/';
      return true;
    default:
      return false;
  }
}

} // end anonymous namespace

FloatPointExceptionChecker::FloatPointExceptionChecker(Context& ctx)
    : Checker(ctx) {}

CheckerName FloatPointExceptionChecker::name() const {
  return CheckerName::FloatPointException;
}

const char* FloatPointExceptionChecker::description() const {
  return "Floating-point exception checker";
}

void FloatPointExceptionChecker::check(ar::Statement* stmt,
                                     const value::AbstractDomain& inv,
                                     CallContext* call_context) {
  if (auto bin = dyn_cast< ar::BinaryOperation >(stmt)) {
    switch (bin->op()) {
      case ar::BinaryOperation::FAdd:
      case ar::BinaryOperation::FSub:
      case ar::BinaryOperation::FMul:
      case ar::BinaryOperation::FDiv:
      case ar::BinaryOperation::FRem:
        this->check_binary(bin, inv, call_context);
        return;
      default:
        return;
    }
  }

  if (auto call = dyn_cast< ar::IntrinsicCall >(stmt)) {
    ar::Function* fun = call->called_function();
    switch (fun->intrinsic_id()) {
      case ar::Intrinsic::FloatSqrt:
      case ar::Intrinsic::FloatLog:
      case ar::Intrinsic::FloatLog2:
      case ar::Intrinsic::FloatLog10:
        this->check_math_call(call, fun->intrinsic_id(), inv, call_context);
        return;
      default:
        return;
    }
  }
}

void FloatPointExceptionChecker::check_binary(ar::BinaryOperation* stmt,
                                            const value::AbstractDomain& inv,
                                            CallContext* call_context) {
  if (inv.is_normal_flow_bottom()) {
    // Statement unreachable
    if (auto msg = this->display_check(Result::Unreachable, stmt)) {
      *msg << "\n";
    }
    this->_checks.insert(CheckKind::Unreachable,
                        CheckerName::FloatPointException, Result::Unreachable,
                        stmt, call_context);
    return;
  }

  const ScalarLit& lhs = this->_lit_factory.get_scalar(stmt->left());
  const ScalarLit& rhs = this->_lit_factory.get_scalar(stmt->right());
  auto operands = std::array< ar::Value*, 2 >{{stmt->left(), stmt->right()}};

  fp::Interval a = operand_interval(lhs, inv);
  fp::Interval b = operand_interval(rhs, inv);

  if (stmt->op() == ar::BinaryOperation::FDiv) {
    if (definitely_zero(b)) {
      if (definitely_zero(a)) {
        // 0/0 raises invalid, not divide-by-zero. Reporting the weaker class
        // here would point the reader at the wrong exception bit.
        this->report(stmt, call_context, Exception::InvalidOperation,
                     Result::Error,
                     "zero divided by zero is an invalid operation", operands);
      } else {
        this->report(stmt, call_context, Exception::DivideByZero, Result::Error,
                     "divisor is definitely zero", operands);
      }
      return;
    }
    if (may_be_zero(b)) {
      this->report(stmt, call_context, Exception::DivideByZero, Result::Warning,
                   "divisor might be zero", operands);
      return;
    }
  }

  if (stmt->op() == ar::BinaryOperation::FRem) {
    // IEEE remainder is undefined for a zero divisor and for an infinite
    // dividend -- invalid, not divide-by-zero.
    if (definitely_zero(b)) {
      this->report(stmt, call_context, Exception::InvalidOperation, Result::Error,
                   "floating-point remainder by zero is invalid", operands);
      return;
    }
    if (may_be_zero(b)) {
      this->report(stmt, call_context, Exception::InvalidOperation,
                   Result::Warning,
                   "floating-point remainder by a possible zero is invalid",
                   operands);
      return;
    }
  }

  // Everything else: is the result pinned to NaN? That is the invalid-operation
  // signature -- `inf - inf`, `inf * 0`, and NaN propagating out of an operand
  // that was itself pinned to NaN upstream.
  char op = '?';
  if (!interval_op_of(stmt->op(), op)) {
    return;
  }

  fp::Interval r = fp::interval_bin_op(op, a, b);
  if (r.is_definitely_nan()) {
    std::string detail =
        std::string("the result of '") + op + "' is definitely NaN";
    this->report(stmt, call_context, Exception::InvalidOperation, Result::Error,
                 detail, operands);
  }
}

void FloatPointExceptionChecker::check_math_call(
    ar::IntrinsicCall* call, ar::Intrinsic::ID id,
    const value::AbstractDomain& inv, CallContext* call_context) {
  if (inv.is_normal_flow_bottom()) {
    if (auto msg = this->display_check(Result::Unreachable, call)) {
      *msg << "\n";
    }
    this->_checks.insert(CheckKind::Unreachable, CheckerName::FloatPointException,
                        Result::Unreachable, call, call_context);
    return;
  }

  if (call->num_arguments() != 1) {
    return;
  }

  const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));
  fp::Interval a = operand_interval(la, inv);

  if (!definitely_negative(a)) {
    // Either the operand can be non-negative, in which case the call is fine on
    // that input, or nothing is known. Neither is a reportable finding.
    return;
  }

  const char* fn = "?";
  switch (id) {
    case ar::Intrinsic::FloatSqrt:
      fn = "sqrt";
      break;
    case ar::Intrinsic::FloatLog:
      fn = "log";
      break;
    case ar::Intrinsic::FloatLog2:
      fn = "log2";
      break;
    case ar::Intrinsic::FloatLog10:
      fn = "log10";
      break;
    default:
      return;
  }

  std::string detail =
      std::string(fn) + " of a definitely-negative operand is invalid";
  this->report(call, call_context, Exception::InvalidOperation, Result::Error,
               detail, std::array< ar::Value*, 1 >{{call->argument(0)}});
}

void FloatPointExceptionChecker::report(ar::Statement* stmt,
                                      CallContext* call_context, Exception exn,
                                      Result result, std::string const& detail,
                                      llvm::ArrayRef< ar::Value* > operands) {
  const char* condition =
      (exn == Exception::DivideByZero) ? kZECondition : kIOCondition;
  const char* klass =
      (exn == Exception::DivideByZero) ? "divide-by-zero" : "invalid-operation";

  if (auto msg = this->display_check(result, stmt)) {
    *msg << "floating-point " << klass << ": " << detail << condition << "\n";
  }

  this->_checks.insert(CheckKind::FloatPointException,
                      CheckerName::FloatPointException, result, stmt, call_context,
                      operands, JsonDict{{"exception", klass}});
}

fp::Interval FloatPointExceptionChecker::operand_interval(
    const ScalarLit& lit, const value::AbstractDomain& inv) {
  if (lit.is_floating_point()) {
    return fp::Interval::point(lit.floating_point());
  }
  if (lit.is_floating_point_var()) {
    if (const FloatingPoint* cst = inv.normal().float_get_cst(lit.var())) {
      return fp::Interval::point(*cst);
    }
    if (const fp::Interval* iv = inv.normal().float_get_interval(lit.var())) {
      return *iv;
    }
  }
  return fp::Interval::top_value();
}

bool FloatPointExceptionChecker::definitely_zero(fp::Interval const& iv) {
  return iv.is_point() && iv.lo.to_double() == 0.0;
}

bool FloatPointExceptionChecker::may_be_zero(fp::Interval const& iv) {
  return !iv.ordered_empty() && iv.lo.to_double() <= 0.0 &&
         iv.hi.to_double() >= 0.0;
}

bool FloatPointExceptionChecker::definitely_negative(fp::Interval const& iv) {
  // A NaN operand counts here as well: sqrt(NaN) and log(NaN) raise the invalid
  // exception just as sqrt(-1) does, so an interval whose ordered part is empty
  // but which is pinned to NaN is still a definite domain error.
  if (iv.is_definitely_nan()) {
    return true;
  }
  return !iv.ordered_empty() && iv.hi.to_double() < 0.0;
}

} // end namespace analyzer
} // end namespace ikos

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

#pragma once

#include <string>

#include <llvm/ADT/ArrayRef.h>

#include <ikos/analyzer/checker/checker.hpp>

namespace ikos {
namespace analyzer {

/// \brief Checker for floating-point operations that raise a hardware FP
/// exception
///
/// IEEE 754 leaves what happens on an exceptional operation up to the
/// implementation. The default substitutes a special value and carries on,
/// which is why a float divide-by-zero is not a defect on a desktop and why
/// this is not folded into `dbz`. That default is not a law: the ARM FPSCR and
/// the x86 MXCSR each carry a per-class exception mask, and a target running
/// with a class unmasked takes a real exception instead of the substitution.
/// On such a target a float divide-by-zero is a crash exactly as an integer one
/// is. Opt-in for that reason -- on a general-purpose target every report here
/// is noise.
///
/// Two exception classes are checked:
///
///   * divide-by-zero -- `fdiv` with a divisor that is definitely zero
///   * invalid operation -- arithmetic that definitely yields a NaN (`0/0`,
///     `inf - inf`, `inf * 0`), `sqrt` of a definitely-negative operand,
///     `frem` by a definitely-zero divisor, and `log`/`log2`/`log10` of a
///     definitely-negative operand
///
/// Deliberately not checked:
///
///   * Overflow to infinity. Measured on InterflopBench it is provable in only
///     22 of 129 real cases, so a report would stay silent exactly when it
///     matters and read as "clean". A checker that is wrong by omission is
///     worse than no checker.
///   * log(0). C raises FE_DIVBYZERO for it as a library convention, but no
///     hardware exception accompanies it -- the hardware returns -inf and moves
///     on. Reporting it here would claim a trap that cannot happen.
///
/// The exception mask itself is configured outside the analyzed code and is
/// invisible to this analysis. Every message is therefore worded conditionally
/// -- "would trap on targets with the exception unmasked" -- never as an
/// unconditional crash claim.
class FloatPointExceptionChecker final : public Checker {
public:
  /// \brief Which IEEE exception class a finding belongs to
  enum class Exception {
    /// FPSCR.DZ / MXCSR.ZE -- `x / 0.0`
    DivideByZero,

    /// FPSCR.ID / MXCSR.IE -- an operation with no representable answer
    InvalidOperation,
  };

  /// \brief Constructor
  explicit FloatPointExceptionChecker(Context& ctx);

  /// \brief Get the checker name
  CheckerName name() const override;

  /// \brief Get the checker description
  const char* description() const override;

  /// \brief Check a statement
  void check(ar::Statement* stmt,
             const value::AbstractDomain& inv,
             CallContext* call_context) override;

private:
  /// \brief Check result
  struct CheckResult {
    CheckKind kind;
    Result result;
    JsonDict info;
  };

  /// \brief Check a floating-point binary operation
  ///
  /// `fdiv` is checked for both classes: a zero divisor is divide-by-zero, and
  /// `0/0` is additionally invalid. The others are checked for invalid only.
  void check_binary(ar::BinaryOperation* stmt, const value::AbstractDomain& inv,
                    CallContext* call_context);

  /// \brief Check a `sqrt` / `log` / `log2` / `log10` call for a domain error
  void check_math_call(ar::IntrinsicCall* call, ar::Intrinsic::ID id,
                       const value::AbstractDomain& inv,
                       CallContext* call_context);

  /// \brief Record a finding
  void report(ar::Statement* stmt, CallContext* call_context, Exception exn,
              Result result, std::string const& detail,
              llvm::ArrayRef< ar::Value* > operands);

  /// \brief Over-approximation of a float operand's value set
  ///
  /// An untracked variable has no map entry and `float_get_interval` returns
  /// null for it. That is TOP -- anything is possible -- not "nothing to say",
  /// so it is materialised as top rather than treated as an absence of work.
  static core::floating_point::Interval operand_interval(
      const ScalarLit& lit, const value::AbstractDomain& inv);

  /// \brief Whether every value the interval allows is a zero (either sign)
  static bool definitely_zero(core::floating_point::Interval const& iv);

  /// \brief Whether some value the interval allows is a zero (either sign)
  static bool may_be_zero(core::floating_point::Interval const& iv);

  /// \brief Whether every value the interval allows raises the sqrt/log domain
  /// error
  ///
  /// NaN counts as negative for this purpose: `sqrt(NaN)` raises the invalid
  /// exception just as `sqrt(-1)` does, so an interval pinned to NaN is still a
  /// definite domain error rather than an unknown one.
  static bool definitely_negative(core::floating_point::Interval const& iv);
};

} // end namespace analyzer
} // end namespace ikos

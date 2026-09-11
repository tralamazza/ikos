/*****************************************************************************
 * The ikos project
 *
 * Copyright (c) 2011-2026 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration. All
 * Rights Reserved.
 *****************************************************************************/

#pragma once

#include <cfenv>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <ikos/core/number/floating_point.hpp>
#include <ikos/core/number/machine_int.hpp>
#include <ikos/core/semantic/dumpable.hpp>
#include <ikos/core/semantic/variable.hpp>

namespace ikos {
namespace core {
namespace floating_point {

/// \brief A closed interval over floating point values, plus NaN possibility
///
/// The ordered range [lo, hi] never contains NaN -- NaN is unordered and cannot
/// be bounded. Whether the value might be NaN at all is tracked separately by
/// `may_nan`. Both parts are needed: without `may_nan` the domain would claim
/// every value is ordered, which would make `ord(x)` provable for an unknown
/// double and therefore unsound.
struct Interval {
  /// \brief Lower bound of the ordered range (may be -inf)
  FloatingPoint lo;

  /// \brief Upper bound of the ordered range (may be +inf)
  FloatingPoint hi;

  /// \brief Can the value be NaN?
  bool may_nan;

  static FloatingPoint neg_inf() {
    return FloatingPoint::from_double(-std::numeric_limits<double>::infinity());
  }

  static FloatingPoint pos_inf() {
    return FloatingPoint::from_double(std::numeric_limits<double>::infinity());
  }

  /// \brief Nothing known: any ordered value, or NaN
  static Interval top_value() {
    return Interval{neg_inf(), pos_inf(), true};
  }

  /// \brief Exactly one known value
  static Interval point(const FloatingPoint& v) {
    if (v.is_nan()) {
      // The ordered range is empty; only NaN remains possible.
      return Interval{pos_inf(), neg_inf(), true};
    }
    return Interval{v, v, false};
  }

  /// \brief The ordered range holds no value
  bool ordered_empty() const {
    return this->lo.to_double() > this->hi.to_double();
  }

  /// \brief Is the value definitely NaN?
  bool is_definitely_nan() const { return this->may_nan && this->ordered_empty(); }

  /// \brief Is the ordered range a single value?
  bool is_point() const { return !this->may_nan && this->lo == this->hi; }

  /// \brief Can the value be anything at all?
  bool is_top() const {
    return this->may_nan && this->lo == neg_inf() && this->hi == pos_inf();
  }

  /// \brief Does the interval contain no value?
  bool is_empty() const { return !this->may_nan && this->ordered_empty(); }

  bool contains(const FloatingPoint& v) const {
    if (v.is_nan()) {
      return this->may_nan;
    }
    return !this->ordered_empty() && this->lo.to_double() <= v.to_double() &&
           v.to_double() <= this->hi.to_double();
  }

  bool contains(const Interval& other) const {
    if (other.may_nan && !this->may_nan) {
      return false;
    }
    if (other.ordered_empty()) {
      return true;
    }
    if (this->ordered_empty()) {
      return false;
    }
    return this->lo.to_double() <= other.lo.to_double() &&
           other.hi.to_double() <= this->hi.to_double();
  }

  bool operator==(const Interval& other) const {
    return this->lo == other.lo && this->hi == other.hi &&
           this->may_nan == other.may_nan;
  }
};

/// \brief RAII switch of the host floating point rounding direction
///
/// Interval arithmetic over uncertain values MUST round outward. A bound computed
/// with round-to-nearest can land strictly inside the true set of concrete
/// results, which would let the analysis prove something false. fesetround is
/// process-global; that is acceptable under the analyzer's documented
/// single-threaded execution, and the guard always restores the previous mode.
class RoundingGuard {
private:
  int _saved;

public:
  explicit RoundingGuard(int mode) {
    this->_saved = std::fegetround();
    if (std::fesetround(mode) != 0) {
      std::fesetround(this->_saved);
      ikos_unreachable("rounding mode is not supported by the host");
    }
  }

  RoundingGuard(const RoundingGuard&) = delete;
  RoundingGuard& operator=(const RoundingGuard&) = delete;
  RoundingGuard(RoundingGuard&&) = delete;
  RoundingGuard& operator=(RoundingGuard&&) = delete;

  ~RoundingGuard() { std::fesetround(this->_saved); }
};

/// \brief Evaluate `a op b` under a fixed rounding direction
///
/// The rounding happens in the host operation; FloatingPoint only carries the
/// resulting bits, so from_double is not itself rounded.
///
/// The result keeps the operands' width. Computing 32-bit values through the host
/// double path would relabel them as binary64, and a binary32 literal would then
/// never compare equal to the value it came from.
inline FloatingPoint round_op(const FloatingPoint& a,
                             const FloatingPoint& b,
                             char op,
                             int mode) {
  RoundingGuard guard(mode);

  if (a.bit_width() == 32 && b.bit_width() == 32) {
    float x = a.to_float();
    float y = b.to_float();
    switch (op) {
    case '+':
      return FloatingPoint::from_float(x + y);
    case '-':
      return FloatingPoint::from_float(x - y);
    case '*':
      return FloatingPoint::from_float(x * y);
    case '/':
      return FloatingPoint::from_float(x / y);
    default:
      ikos_unreachable("not a floating point binary operator");
    }
  }

  double x = a.to_double();
  double y = b.to_double();
  switch (op) {
  case '+':
    return FloatingPoint::from_double(x + y);
  case '-':
    return FloatingPoint::from_double(x - y);
  case '*':
    return FloatingPoint::from_double(x * y);
  case '/':
    return FloatingPoint::from_double(x / y);
  default:
    ikos_unreachable("not a floating point binary operator");
  }
}

/// \brief Interval arithmetic: `[a,b] op [c,d]`
///
/// Two known constants have no uncertainty to widen over: the program computes
/// exactly the round-to-nearest result, so the honest answer is that single
/// value. Widening there would turn every propagated constant into a range and
/// lose the exact-equality proofs.
///
/// With any uncertainty the four corners are evaluated, the minimum with
/// round-toward-negative-infinity and the maximum with round-toward-positive-
/// infinity, so the result soundly over-approximates every concrete outcome.
/// A NaN corner, or a divisor spanning zero, makes the result possibly-NaN.
inline Interval interval_bin_op(char op, const Interval& x, const Interval& y) {
  bool divides_zero = op == '/' && !y.ordered_empty() &&
                     y.lo.to_double() <= 0.0 && y.hi.to_double() >= 0.0;
  if (divides_zero) {
    return Interval::top_value();
  }

  if (x.is_point() && y.is_point()) {
    return Interval::point(round_op(x.lo, y.lo, op, FE_TONEAREST));
  }

  // NaN in either operand propagates through every arithmetic operation.
  bool may_nan = x.may_nan || y.may_nan;

  if (x.ordered_empty() || y.ordered_empty()) {
    // No ordered values to combine; only the NaN possibility carries over.
    if (!may_nan) {
      return Interval{Interval::pos_inf(), Interval::neg_inf(), false};
    }
    return Interval::top_value();
  }

  const FloatingPoint xs[2] = {x.lo, x.hi};
  const FloatingPoint ys[2] = {y.lo, y.hi};

  FloatingPoint lo = Interval::neg_inf();
  FloatingPoint hi = Interval::pos_inf();
  bool have = false;

  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      FloatingPoint down = round_op(xs[i], ys[j], op, FE_DOWNWARD);
      FloatingPoint up = round_op(xs[i], ys[j], op, FE_UPWARD);
      if (down.is_nan() || up.is_nan()) {
        may_nan = true;
        continue;
      }
      if (!have) {
        lo = down;
        hi = up;
        have = true;
        continue;
      }
      if (down.to_double() < lo.to_double()) {
        lo = down;
      }
      if (up.to_double() > hi.to_double()) {
        hi = up;
      }
    }
  }

  if (!have) {
    if (!may_nan) {
      return Interval{Interval::pos_inf(), Interval::neg_inf(), false};
    }
    return Interval::top_value();
  }
  return Interval{lo, hi, may_nan};
}

/// \brief Three-valued answer to "does this predicate definitely hold?"
enum class Holds { Yes, No, Might };

/// \brief Evaluate an IEEE predicate over two intervals
///
/// Yes only when every pair of values the intervals allow satisfies the
/// predicate, No only when no pair does, Might otherwise.
///
/// The ordered (o*) predicates mean "ordered AND <cmp>". They are NOT the
/// negation of their unordered counterparts: with a NaN operand `x < y` is false
/// but `x >= y` is false too, not true. Treating the ordered-empty case as
/// "nothing satisfies the comparison" therefore made `oge` and `ole` answer Yes
/// for NaN, which is unsound. NaN is handled by branching on it up front rather
/// than folding it into the range tests.
inline Holds interval_holds(IEEEPredicate pred,
                           const Interval& x,
                           const Interval& y) {
  const bool nan = x.may_nan || y.may_nan;
  const bool def_nan = x.is_definitely_nan() || y.is_definitely_nan();

  // ORD and UNO are about NaN itself, not about a comparison.
  if (pred == IEEEPredicate::ORD) {
    if (def_nan) {
      return Holds::No;
    }
    return nan ? Holds::Might : Holds::Yes;
  }
  if (pred == IEEEPredicate::UNO) {
    if (def_nan) {
      return Holds::Yes;
    }
    return nan ? Holds::Might : Holds::No;
  }

  const bool no_pairs = x.ordered_empty() || y.ordered_empty();
  if (no_pairs && !def_nan) {
    // One side holds no ordered value and is not pinned to NaN either, so it is
    // empty. Nothing can be concluded safely.
    return Holds::Might;
  }

  // Range tests over the ordered parts only.
  const bool all_lt = !no_pairs && x.hi.to_double() < y.lo.to_double();
  const bool all_gt = !no_pairs && x.lo.to_double() > y.hi.to_double();
  const bool none_lt = no_pairs || !(x.lo.to_double() < y.hi.to_double());
  const bool none_gt = no_pairs || !(x.hi.to_double() > y.lo.to_double());
  const bool same_point = x.is_point() && y.is_point() && x.lo == y.hi;

  // What the predicate does over ordered operand pairs. The unordered (u*)
  // spellings share this because they differ only in also accepting NaN.
  Holds ord;
  switch (pred) {
  case IEEEPredicate::OEQ:
  case IEEEPredicate::UEQ:
    ord = same_point ? Holds::Yes : ((all_lt || all_gt) ? Holds::No : Holds::Might);
    break;
  case IEEEPredicate::ONE:
  case IEEEPredicate::UNE:
    ord = (all_lt || all_gt) ? Holds::Yes : (same_point ? Holds::No : Holds::Might);
    break;
  case IEEEPredicate::OLT:
  case IEEEPredicate::ULT:
    ord = all_lt ? Holds::Yes : (none_lt ? Holds::No : Holds::Might);
    break;
  case IEEEPredicate::OLE:
  case IEEEPredicate::ULE:
    ord = none_gt ? Holds::Yes : (all_gt ? Holds::No : Holds::Might);
    break;
  case IEEEPredicate::OGT:
  case IEEEPredicate::UGT:
    ord = all_gt ? Holds::Yes : (none_gt ? Holds::No : Holds::Might);
    break;
  case IEEEPredicate::OGE:
  case IEEEPredicate::UGE:
    ord = none_lt ? Holds::Yes : (all_lt ? Holds::No : Holds::Might);
    break;
  default:
    ord = Holds::Might;
    break;
  }

  const bool ordered_pred = pred == IEEEPredicate::OEQ ||
                           pred == IEEEPredicate::ONE ||
                           pred == IEEEPredicate::OLT ||
                           pred == IEEEPredicate::OLE ||
                           pred == IEEEPredicate::OGT ||
                           pred == IEEEPredicate::OGE;

  if (ordered_pred) {
    if (def_nan) {
      // Every concrete value is NaN, so the ordered predicate never holds.
      return Holds::No;
    }
    if (nan) {
      // The NaN case is false, so a definite Yes is impossible; the ordered
      // case still decides whether anything could hold at all.
      return ord == Holds::No ? Holds::No : Holds::Might;
    }
    return ord;
  }

  // Unordered predicates: additionally satisfied by any NaN operand.
  if (def_nan) {
    return Holds::Yes;
  }
  if (nan && ord != Holds::Yes) {
    return Holds::Might;
  }
  return ord;
}

} // end namespace floating_point

namespace scalar {

using ikos::core::floating_point::Holds;
using ikos::core::floating_point::Interval;

/// \brief Floating point interval domain
///
/// Tracks a closed interval plus a NaN possibility per floating point variable.
/// Absence of an entry means "nothing known". This subsumes constant
/// propagation: a degenerate interval with equal bounds and no NaN possibility
/// is a constant.
///
/// Arithmetic over uncertain values rounds outward (see
/// floating_point::RoundingGuard) so bounds never exclude a value the program
/// can actually produce. Widening pushes a bound to infinity as soon as it
/// moves outward, which makes the descending chain of successive widenings
/// finite and the fixpoint guaranteed.
template < typename VariableRef >
class FloatIntervalDomain {
  static_assert(core::IsVariable< VariableRef >::value,
                "VariableRef does not meet the requirements for variable types");

public:
  using FloatInterval = Interval;

private:
  using Map = std::unordered_map< VariableRef, Interval >;

private:
  /// \brief Bottom flag
  bool _is_bottom;

  /// \brief Known intervals; meaningless when bottom
  Map _iv;

public:
  FloatIntervalDomain() : _is_bottom(false) {}

  FloatIntervalDomain(const FloatIntervalDomain&) = default;
  FloatIntervalDomain(FloatIntervalDomain&&) = default;
  FloatIntervalDomain& operator=(const FloatIntervalDomain&) = default;
  FloatIntervalDomain& operator=(FloatIntervalDomain&&) = default;
  ~FloatIntervalDomain() = default;

  /// \name Core abstract domain methods
  /// @{

  void normalize() {}

  bool is_bottom() const { return this->_is_bottom; }

  bool is_top() const { return !this->_is_bottom && this->_iv.empty(); }

  void set_to_bottom() {
    this->_is_bottom = true;
    this->_iv.clear();
  }

  void set_to_top() {
    this->_is_bottom = false;
    this->_iv.clear();
  }

  bool equals(const FloatIntervalDomain& other) const {
    if (this->is_bottom() || other.is_bottom()) {
      return this->is_bottom() == other.is_bottom();
    }
    return this->_iv == other._iv;
  }

  bool leq(const FloatIntervalDomain& other) const {
    if (this->is_bottom()) {
      return true;
    }
    if (other.is_bottom()) {
      return false;
    }

    // Iterate the OTHER side. `this <= other` requires every variable `other`
    // constrains to be constrained here too: if `other` bounds a variable that we
    // do not track, we are top for it and top is not below a concrete interval.
    // Iterating only our own map would return true vacuously for a top domain.
    for (const auto& kv : other._iv) {
      auto it = this->_iv.find(kv.first);
      if (it == this->_iv.end()) {
        return false;
      }
      if (!kv.second.contains(it->second)) {
        return false;
      }
    }
    return true;
  }

  /// @}
  /// \name Join
  ///
  /// Least upper bound: bound-wise hull.
  /// @{

  void join_with(const FloatIntervalDomain& other) {
    if (this->is_bottom()) {
      this->operator=(other);
      return;
    }
    if (other.is_bottom()) {
      return;
    }

    // Absence means top. A variable tracked here but not in `other` therefore
    // joins with top and must leave the map; keeping our own bound would claim a
    // precision the other branch never had. This mirrors how the int side does it
    // through SeparateDomain, which intersects the two variable trees.
    Map merged;
    for (const auto& kv : this->_iv) {
      auto it = other._iv.find(kv.first);
      if (it == other._iv.end()) {
        continue;
      }
      Interval h = hull(kv.second, it->second);
      if (!h.is_top()) {
        merged.emplace(kv.first, h);
      }
    }
    this->_iv = std::move(merged);
  }

  void join_with(FloatIntervalDomain&& other) {
    this->join_with(static_cast<const FloatIntervalDomain&>(other));
  }

  void join_loop_with(const FloatIntervalDomain& other) {
    this->join_with(other);
  }

  void join_loop_with(FloatIntervalDomain&& other) {
    this->join_with(static_cast<const FloatIntervalDomain&>(other));
  }

  void join_iter_with(const FloatIntervalDomain& other) {
    this->join_with(other);
  }

  void join_iter_with(FloatIntervalDomain&& other) {
    this->join_with(static_cast<const FloatIntervalDomain&>(other));
  }

  /// @}
  /// \name Widening
  ///
  /// A bound that has moved outward is pushed straight to the infinity on that
  /// side. Each variable's lower bound can only take values from
  /// {old, -inf} and its upper bound from {old, +inf}, so repeated widening
  /// cannot descend forever.
  /// @{

  void widen_with(const FloatIntervalDomain& other) {
    if (this->is_bottom()) {
      this->operator=(other);
      return;
    }
    if (other.is_bottom()) {
      return;
    }

    // Same rule as the join: widening a tracked variable against an absent one is
    // widening against top, which is top, so the variable leaves the map.
    Map merged;
    for (const auto& kv : this->_iv) {
      auto it = other._iv.find(kv.first);
      if (it == other._iv.end()) {
        continue;
      }
      const Interval& old_iv = kv.second;
      const Interval& new_iv = it->second;

      FloatingPoint lo = old_iv.lo;
      FloatingPoint hi = old_iv.hi;
      if (!new_iv.ordered_empty() &&
          (old_iv.ordered_empty() ||
           new_iv.lo.to_double() < old_iv.lo.to_double())) {
        lo = Interval::neg_inf();
      }
      if (!new_iv.ordered_empty() &&
          (old_iv.ordered_empty() ||
           new_iv.hi.to_double() > old_iv.hi.to_double())) {
        hi = Interval::pos_inf();
      }
      Interval w{lo, hi, old_iv.may_nan || new_iv.may_nan};
      if (!w.is_top()) {
        merged.emplace(kv.first, std::move(w));
      }
    }
    this->_iv = std::move(merged);
  }

  void widen_with(FloatIntervalDomain&& other) {
    this->widen_with(static_cast<const FloatIntervalDomain&>(other));
  }

  void widen_threshold_with(const FloatIntervalDomain& other, const MachineInt&) {
    this->widen_with(other);
  }

  /// @}
  /// \name Meeting
  ///
  /// Bound-wise intersection.
  /// @{

  void meet_with(const FloatIntervalDomain& other) {
    if (this->is_bottom()) {
      return;
    }
    if (other.is_bottom()) {
      this->set_to_bottom();
      return;
    }
    for (const auto& kv : other._iv) {
      auto it = this->_iv.find(kv.first);
      Interval merged = (it == this->_iv.end()) ? kv.second
                                              : intersect(it->second, kv.second);
      if (merged.is_empty()) {
        this->set_to_bottom();
        return;
      }
      if (it == this->_iv.end()) {
        this->_iv.emplace(kv.first, merged);
      }
      else {
        it->second = merged;
      }
    }
  }

  /// @}
  /// \name Narrowing
  ///
  /// Meet without the bottom short-circuit surprises: an interval we cannot
  /// refine simply stays as it is.
  /// @{

  void narrow_with(const FloatIntervalDomain& other) {
    if (this->is_bottom() || other.is_bottom()) {
      this->set_to_bottom();
      return;
    }
    this->meet_with(other);
  }

  void narrow_threshold_with(const FloatIntervalDomain& other, const MachineInt&) {
    this->narrow_with(other);
  }

  /// @}
  /// \name Floating point operations
  /// @{

  /// \brief `x = cst`
  void assign(VariableRef x, const FloatingPoint& cst) {
    if (this->is_bottom()) {
      return;
    }
    if (!cst.is_modeled()) {
      this->_iv.erase(x);
      return;
    }
    this->_iv.insert_or_assign(x, Interval::point(cst));
  }

  /// \brief `x = y`
  void assign(VariableRef x, VariableRef y) {
    if (this->is_bottom()) {
      return;
    }
    auto it = this->_iv.find(y);
    if (it == this->_iv.end()) {
      this->_iv.erase(x);
      return;
    }
    Interval v = it->second;
    this->_iv.insert_or_assign(x, v);
  }

  /// \brief Forget everything about `x`
  void forget(VariableRef x) {
    if (!this->is_bottom()) {
      this->_iv.erase(x);
    }
  }

  /// \brief The known interval for `x`, or nullptr if nothing is known
  const Interval* get_interval(VariableRef x) const {
    if (this->is_bottom()) {
      return nullptr;
    }
    auto it = this->_iv.find(x);
    return it == this->_iv.end() ? nullptr : &it->second;
  }

  /// \brief The exact constant for `x`, or nullptr
  const FloatingPoint* get(VariableRef x) const {
    const Interval* iv = this->get_interval(x);
    if (iv == nullptr || !iv->is_point()) {
      return nullptr;
    }
    return &iv->lo;
  }

  /// \brief Is `x` a known constant?
  bool is_constant(VariableRef x) const { return this->get(x) != nullptr; }

  /// \brief Set `x` to a known interval
  void apply_interval(VariableRef x, const Interval& iv) {
    if (this->is_bottom()) {
      return;
    }
    if (iv.is_empty()) {
      this->set_to_bottom();
      return;
    }
    if (iv.is_top() || !iv.lo.is_modeled() || !iv.hi.is_modeled()) {
      this->_iv.erase(x);
      return;
    }
    this->_iv.insert_or_assign(x, iv);
  }

  /// \brief Constrain `x` by `x pred cst`
  void refine(VariableRef x, IEEEPredicate pred, const FloatingPoint& cst) {
    if (this->is_bottom() || !cst.is_modeled()) {
      return;
    }
    Interval cur = this->_current(x);
    Interval next = constrain(cur, pred, cst);
    if (next.is_empty()) {
      this->set_to_bottom();
      return;
    }
    this->apply_interval(x, next);
  }

  /// \brief Constrain `x` by `cst pred x` (predicate read from the other side)
  void refine_flipped(VariableRef x, IEEEPredicate pred,
                     const FloatingPoint& cst) {
    this->refine(x, flip(pred), cst);
  }

  /// @}

  /// \name Value-returning combinators
  /// @{

  FloatIntervalDomain join(const FloatIntervalDomain& other) const {
    FloatIntervalDomain res = *this;
    res.join_with(other);
    return res;
  }

  FloatIntervalDomain join_loop(const FloatIntervalDomain& other) const {
    FloatIntervalDomain res = *this;
    res.join_loop_with(other);
    return res;
  }

  FloatIntervalDomain join_iter(const FloatIntervalDomain& other) const {
    FloatIntervalDomain res = *this;
    res.join_iter_with(other);
    return res;
  }

  FloatIntervalDomain widening(const FloatIntervalDomain& other) const {
    FloatIntervalDomain res = *this;
    res.widen_with(other);
    return res;
  }

  FloatIntervalDomain widening_threshold(const FloatIntervalDomain& other,
                                        const MachineInt& threshold) const {
    FloatIntervalDomain res = *this;
    res.widen_threshold_with(other, threshold);
    return res;
  }

  FloatIntervalDomain meet(const FloatIntervalDomain& other) const {
    FloatIntervalDomain res = *this;
    res.meet_with(other);
    return res;
  }

  FloatIntervalDomain narrowing(const FloatIntervalDomain& other) const {
    FloatIntervalDomain res = *this;
    res.narrow_with(other);
    return res;
  }

  FloatIntervalDomain narrowing_threshold(const FloatIntervalDomain& other,
                                         const MachineInt& threshold) const {
    FloatIntervalDomain res = *this;
    res.narrow_threshold_with(other, threshold);
    return res;
  }

  /// @}

  /// \brief Dump for debugging
  void dump(std::ostream& o) const {
    if (this->is_bottom()) {
      o << "float_iv: bottom";
      return;
    }
    o << "float_iv: {";
    bool first = true;
    for (const auto& kv : this->_iv) {
      if (!first) {
        o << ", ";
      }
      first = false;
      DumpableTraits< VariableRef >::dump(o, kv.first);
      o << " -> [" << kv.second.lo << ", " << kv.second.hi << "]";
      if (kv.second.may_nan) {
        o << "|nan";
      }
    }
    o << "}";
  }

  friend std::ostream& operator<<(std::ostream& o, const FloatIntervalDomain& d) {
    d.dump(o);
    return o;
  }

private:
  /// \brief Current interval for `x`, defaulting to the whole line
  Interval _current(VariableRef x) const {
    auto it = this->_iv.find(x);
    return it == this->_iv.end() ? Interval::top_value() : it->second;
  }

  /// \brief The next float of `f`'s own width toward +/-infinity.
  ///
  /// The step has to happen AT the value's width: computing in double and then
  /// narrowing would be double rounding and would not land on the adjacent float
  /// (the run #28 width lesson).  An unmodeled width returns `f` unchanged so a
  /// caller degrades to the old looser bound rather than to a wrong one.
  static FloatingPoint step(const FloatingPoint& f, bool upward) {
    if (!f.is_modeled()) {
      return f;
    }
    if (f.bit_width() == 32) {
      const float bound = upward ? std::numeric_limits<float>::infinity()
                               : -std::numeric_limits<float>::infinity();
      return FloatingPoint::from_float(std::nextafterf(f.to_float(), bound));
    }
    if (f.bit_width() == 64) {
      const double bound = upward ? std::numeric_limits<double>::infinity()
                                : -std::numeric_limits<double>::infinity();
      return FloatingPoint::from_double(std::nextafter(f.to_double(), bound));
    }
    return f;
  }

  /// \brief Apply one IEEE predicate as a constraint on `iv`
  ///
  /// Where an interval cannot express the constraint exactly (`!=`, the strict
  /// inequalities as upper bounds) we keep the looser bound. Over-approximating
  /// is always sound; under-approximating is not.
  static Interval constrain(const Interval& iv,
                           IEEEPredicate pred,
                           const FloatingPoint& c) {
    const Interval bot_nan{Interval::pos_inf(), Interval::neg_inf(), true};
    const Interval bot_ord{Interval::pos_inf(), Interval::neg_inf(), false};

    if (c.is_nan()) {
      // With a NaN constant the unordered predicates say nothing at all. `x
      // u<cmp> NaN` unfolds to `unordered(x, NaN) OR (x <cmp> NaN)`, and
      // `unordered(x, NaN)` holds because NaN is one of the operands. So the
      // constraint is satisfied by every x, and pinning x to NaN would drop
      // perfectly legal ordered values.
      //
      // Every ordered predicate against NaN is false, so those really are
      // unsatisfiable.
      const bool unordered = pred == IEEEPredicate::UNO ||
                            pred == IEEEPredicate::UEQ ||
                            pred == IEEEPredicate::UNE ||
                            pred == IEEEPredicate::UGT ||
                            pred == IEEEPredicate::UGE ||
                            pred == IEEEPredicate::ULT ||
                            pred == IEEEPredicate::ULE;
      if (unordered) {
        return iv;
      }
      return bot_ord;
    }

    switch (pred) {
    case IEEEPredicate::OEQ:
      return Interval::point(c);
    case IEEEPredicate::ONE:
      // The hole `x != c` cannot be expressed by an interval, so the bounds stay.
      // But `one` is the ORDERED not-equal: it holds only when the operands are
      // ordered, so NaN is ruled out.  The old code returned `iv` untouched,
      // which kept may_nan set and lost that fact -- losing it also blocks the
      // run #46 same-variable identities (x-x, x/x), which key on may_nan.
      return Interval{iv.lo, iv.hi, false};
    case IEEEPredicate::ULE:
      return Interval{iv.lo, c, iv.may_nan};
    case IEEEPredicate::OLT:
      // Nothing is strictly below -infinity, so this is unsatisfiable rather
      // than a bound of -infinity.
      if (c.is_inf() && c.to_double() < 0.0) {
        return bot_ord;
      }
      // The largest float strictly below c is its neighbour, so that neighbour
      // is the honest upper bound.  Keeping `c` itself threw away a full ULP on
      // every strict comparison.
      return Interval{iv.lo, step(c, false), false};
    case IEEEPredicate::OGE:
      return Interval{c, iv.hi, false};
    case IEEEPredicate::OGT:
      // Nothing is strictly above +infinity.
      if (c.is_inf() && c.to_double() > 0.0) {
        return bot_ord;
      }
      return Interval{step(c, true), iv.hi, false};
    case IEEEPredicate::OLE:
      return Interval{iv.lo, c, false};
    case IEEEPredicate::UEQ:
      // Either NaN or equal; we cannot express the NaN alternative.
      return iv;
    case IEEEPredicate::UNE:
    case IEEEPredicate::UGT:
    case IEEEPredicate::UGE:
    case IEEEPredicate::ULT:
      // Satisfied by the ordered part or by NaN; keep NaN possible.
      return iv;
    case IEEEPredicate::ORD:
      return Interval{iv.lo, iv.hi, false};
    case IEEEPredicate::UNO:
      return bot_nan;
    default:
      return iv;
    }
  }

  /// \brief Mirror a predicate so `cst pred x` becomes `x mirror(pred) cst`
  static IEEEPredicate flip(IEEEPredicate pred) {
    switch (pred) {
    case IEEEPredicate::OLT:
      return IEEEPredicate::OGT;
    case IEEEPredicate::OGE:
      return IEEEPredicate::OLE;
    case IEEEPredicate::OGT:
      return IEEEPredicate::OLT;
    case IEEEPredicate::OLE:
      return IEEEPredicate::OGE;
    case IEEEPredicate::ULT:
      return IEEEPredicate::UGT;
    case IEEEPredicate::UGE:
      return IEEEPredicate::ULE;
    case IEEEPredicate::UGT:
      return IEEEPredicate::ULT;
    case IEEEPredicate::ULE:
      return IEEEPredicate::UGE;
    default:
      return pred;
    }
  }

  /// \brief Bound-wise hull
  ///
  /// The NaN possibility is the union of both sides and must survive even when one
  /// side has no ordered part. Returning the other operand verbatim for an
  /// ordered-empty input dropped that side's `may_nan`, which made the join fail
  /// to be an upper bound of its own operand: `join(NaN, [-26,47])` came back as
  /// `[-26,47]` with no NaN, so the analysis could then prove `x >= 0` about a
  /// value that is actually NaN.
  static Interval hull(const Interval& a, const Interval& b) {
    const bool nan = a.may_nan || b.may_nan;
    const bool a_empty = a.ordered_empty();
    const bool b_empty = b.ordered_empty();

    if (a_empty && b_empty) {
      return Interval{Interval::pos_inf(), Interval::neg_inf(), nan};
    }
    if (a_empty) {
      return Interval{b.lo, b.hi, nan};
    }
    if (b_empty) {
      return Interval{a.lo, a.hi, nan};
    }

    FloatingPoint lo = a.lo.to_double() <= b.lo.to_double() ? a.lo : b.lo;
    FloatingPoint hi = a.hi.to_double() >= b.hi.to_double() ? a.hi : b.hi;
    return Interval{lo, hi, nan};
  }

  /// \brief Bound-wise intersection
  static Interval intersect(const Interval& a, const Interval& b) {
    bool may_nan = a.may_nan && b.may_nan;

    if (a.ordered_empty() || b.ordered_empty()) {
      return Interval{Interval::pos_inf(), Interval::neg_inf(), may_nan};
    }
    FloatingPoint lo = a.lo.to_double() >= b.lo.to_double() ? a.lo : b.lo;
    FloatingPoint hi = a.hi.to_double() <= b.hi.to_double() ? a.hi : b.hi;
    if (lo.to_double() > hi.to_double()) {
      return Interval{Interval::pos_inf(), Interval::neg_inf(), may_nan};
    }
    return Interval{lo, hi, may_nan};
  }
};

} // end namespace scalar
} // end namespace core
} // end namespace ikos

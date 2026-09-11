/*****************************************************************************
 * The ikos project
 *
 * Copyright (c) 2011-2026 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration. All
 * Rights Reserved.
 *****************************************************************************/

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ostream>

#include <ikos/core/support/assert.hpp>

namespace ikos {
namespace core {

/// \brief IEEE-754 comparison predicates
///
/// Mirrors the ordered (o*) and unordered (u*) predicates of the LLVM fcmp
/// instruction. The distinction only matters when an operand is NaN: an ordered
/// predicate is false, an unordered one is true.
enum class IEEEPredicate : uint8_t {
  OEQ, ///< ordered equal
  OGT, ///< ordered greater than
  OGE, ///< ordered greater or equal
  OLT, ///< ordered less than
  OLE, ///< ordered less or equal
  ONE, ///< ordered not equal
  ORD, ///< ordered (neither operand is NaN)
  UNO, ///< unordered (at least one operand is NaN)
  UEQ, ///< unordered equal
  UGT, ///< unordered greater than
  UGE, ///< unordered greater or equal
  ULT, ///< unordered less than
  ULE, ///< unordered less or equal
  UNE, ///< unordered not equal
};

/// \brief Floating point value with IEEE-754 semantics
///
/// Only the widths the host can model exactly -- binary32 and binary64 -- are
/// represented as values. Every other width (binary16, x87 80-bit, binary128)
/// is carried as `unmodeled`, which forces callers to top out rather than guess
/// at semantics we cannot reproduce. This is deliberate: a wrong floating point
/// proof is worse than no proof.
///
/// Value identity is bit-pattern identity. That is NOT the same thing as the
/// IEEE-754 `==` operator, which reports NaN as unequal to itself. Use
/// `ieee_equal` for the latter. Keeping these separate is what makes constant
/// propagation sound while comparisons stay honest about NaN.
class FloatingPoint {
public:
  /// \brief Whether this width can be modeled exactly on this host
  enum Kind : uint8_t { Modeled, Unmodeled };

private:
  /// \brief Value kind
  Kind _kind;

  /// \brief Total bit width (16, 32, 64, 80, 128, ...)
  uint16_t _bit_width;

  /// \brief IEEE-754 bit pattern; meaningful only when _kind == Modeled
  uint64_t _bits;

private:
  FloatingPoint(Kind kind, uint16_t bit_width, uint64_t bits)
      : _kind(kind), _bit_width(bit_width), _bits(bits) {}

public:
  /// \brief An unmodeled floating point of the given width
  ///
  /// Callers must treat this as non-constant and top out.
  static FloatingPoint unmodeled(uint16_t bit_width) {
    return FloatingPoint(Unmodeled, bit_width, 0);
  }

  /// \brief Build a binary32 from a native float
  static FloatingPoint from_float(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return FloatingPoint(Modeled, 32, bits);
  }

  /// \brief Build a binary64 from a native double
  static FloatingPoint from_double(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return FloatingPoint(Modeled, 64, bits);
  }

  /// \brief Build from a raw bit pattern of a known width
  ///
  /// Only 32 and 64 bit widths are modeled; anything else comes back unmodeled.
  static FloatingPoint from_bits(uint16_t bit_width, uint64_t bits) {
    if (bit_width == 32 || bit_width == 64) {
      return FloatingPoint(Modeled, bit_width, bits);
    }
    return unmodeled(bit_width);
  }

  /// \brief Is this width modeled exactly on this host?
  bool is_modeled() const { return this->_kind == Modeled; }

  /// \brief Is this a value we cannot reason about?
  bool is_unmodeled() const { return this->_kind == Unmodeled; }

  /// \brief Bit width
  uint16_t bit_width() const { return this->_bit_width; }

  /// \brief Raw IEEE-754 bit pattern
  ///
  /// Only meaningful when is_modeled().
  uint64_t bits() const { return this->_bits; }

  /// \brief Reinterpret the bit pattern as a native float
  float to_float() const {
    float f = 0.0f;
    uint32_t b = static_cast<uint32_t>(this->_bits);
    std::memcpy(&f, &b, sizeof(f));
    return f;
  }

  /// \brief Reinterpret the bit pattern as a native double
  double to_double() const {
    double d = 0.0;
    if (this->_bit_width == 32) {
      float f = this->to_float();
      d = static_cast<double>(f);
    }
    else {
      std::memcpy(&d, &this->_bits, sizeof(d));
    }
    return d;
  }

  /// \name Classification
  /// @{

  /// \brief Is this NaN?
  bool is_nan() const {
    if (this->_bit_width == 32) {
      return std::isnan(this->to_float());
    }
    return std::isnan(this->to_double());
  }

  /// \brief Is this +inf or -inf?
  bool is_inf() const {
    if (this->_bit_width == 32) {
      return std::isinf(this->to_float());
    }
    return std::isinf(this->to_double());
  }

  /// \brief Is this zero (either sign)?
  bool is_zero() const { return is_signed_zero_bits(*this); }

  /// \brief Reinterpret the same value in a different binary width
  ///
  /// Widening (fpext) is exact. Narrowing (fptrunc) rounds to nearest even in
  /// the destination width and overflows to infinity, both of which the host
  /// cast does for us. Returns an unmodeled value when the destination width is
  /// not one we can represent.
  FloatingPoint cast_width(uint16_t dst) const {
    if (!this->is_modeled()) {
      return FloatingPoint::unmodeled(dst);
    }
    if (dst == this->_bit_width) {
      return *this;
    }
    if (dst == 32) {
      return FloatingPoint::from_float(static_cast<float>(this->to_double()));
    }
    if (dst == 64) {
      return FloatingPoint::from_double(static_cast<double>(this->to_double()));
    }
    return FloatingPoint::unmodeled(dst);
  }

  /// \brief Truncate toward zero, as fptosi/fptoui do
  ///
  /// Returns NaN when there is no integral value to extract. Callers must
  /// range-check the result against the target integer width; out of range is
  /// undefined behaviour in C, so they top out rather than pick a value.
  double trunc_toward_zero() const {
    if (this->is_nan() || this->is_inf()) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return std::trunc(this->to_double());
  }

  /// \brief Sign bit
  bool signbit() const {
    if (this->_bit_width == 32) {
      return (this->_bits >> 31) & 1u;
    }
    return (this->_bits >> 63) & 1ull;
  }

  /// @}
  /// \name IEEE-754 arithmetic
  ///
  /// Both operands must be modeled and of the same width. The operation is
  /// performed by the host in that width, so rounding matches what the program
  /// would do.
  /// @{

  static FloatingPoint add(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    if (a._bit_width == 32) {
      return from_float(a.to_float() + b.to_float());
    }
    return from_double(a.to_double() + b.to_double());
  }

  static FloatingPoint sub(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    if (a._bit_width == 32) {
      return from_float(a.to_float() - b.to_float());
    }
    return from_double(a.to_double() - b.to_double());
  }

  static FloatingPoint mul(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    if (a._bit_width == 32) {
      return from_float(a.to_float() * b.to_float());
    }
    return from_double(a.to_double() * b.to_double());
  }

  static FloatingPoint div(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    if (a._bit_width == 32) {
      return from_float(a.to_float() / b.to_float());
    }
    return from_double(a.to_double() / b.to_double());
  }

  /// \brief Unary negation
  static FloatingPoint neg(const FloatingPoint& a) {
    return FloatingPoint(Modeled, a._bit_width,
                        a._bits ^ (uint64_t(1) << msb_of(a._bit_width)));
  }

  /// \brief Absolute value: clear the sign bit.
  ///
  /// Done on the encoding rather than via a native fabs, so it is exact at every
  /// width with no rounding and no chance of relabelling a narrow value as a
  /// wider one. A NaN keeps its payload and -0.0 becomes +0.0, both matching
  /// IEEE-754.
  static FloatingPoint abs(const FloatingPoint& a) {
    if (!a.is_modeled()) {
      return a;
    }
    return FloatingPoint(Modeled, a._bit_width,
                        a._bits & ~(uint64_t(1) << msb_of(a._bit_width)));
  }

  /// \brief Magnitude of `a` carrying the sign bit of `b`.
  ///
  /// Pure bit manipulation, so it is exact at every width. Note the sign comes
  /// from `b`'s sign BIT regardless of what `b` is: copysign(2.5, -0.0) is
  /// -2.5, and copysign(2.5, -NaN) is -2.5 too. Hardware-verified.
  ///
  /// The magnitude, and therefore any NaN-ness, comes from `a`: copysign of a
  /// NaN magnitude is still NaN. `b` can never introduce a NaN.
  static FloatingPoint copysign(const FloatingPoint& a, const FloatingPoint& b) {
    if (!a.is_modeled() || !b.is_modeled()) {
      return unmodeled(a._bit_width);
    }
    uint64_t mask = uint64_t(1) << msb_of(a._bit_width);
    return FloatingPoint(Modeled, a._bit_width,
                        (a._bits & ~mask) | (b._bits & mask));
  }

  /// @}
  /// \name IEEE-754 comparisons
  ///
  /// These return the result the program would observe, including the NaN rules:
  /// every ordered comparison involving NaN is false, and `!=` against NaN is
  /// true. Both operands must be modeled and of the same width.
  /// @{

  static bool ieee_equal(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    // Done on bit patterns rather than a native `==` so that -Wfloat-equal stays
    // enabled everywhere else, and so the NaN rule is explicit: NaN is never
    // equal to anything, including itself.
    if (a.is_nan() || b.is_nan()) {
      return false;
    }
    // +0.0 and -0.0 compare equal but have different bit patterns.
    return a._bits == b._bits ||
           (is_signed_zero_bits(a) && is_signed_zero_bits(b));
  }

  static bool ieee_not_equal(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    return !ieee_equal(a, b);
  }

  static bool ieee_greater(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    return a.to_double() > b.to_double();
  }

  static bool ieee_greater_equal(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    return a.to_double() >= b.to_double();
  }

  static bool ieee_less(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    return a.to_double() < b.to_double();
  }

  static bool ieee_less_equal(const FloatingPoint& a, const FloatingPoint& b) {
    require_compatible(a, b);
    return a.to_double() <= b.to_double();
  }

  /// @}

  /// \brief Value identity: same width and same bit pattern
  ///
  /// Note this is NOT the IEEE `==`. Two NaNs with the same payload are equal
  /// here, because for constant propagation they are the same constant.
  bool operator==(const FloatingPoint& o) const {
    return this->_kind == o._kind && this->_bit_width == o._bit_width &&
           this->_bits == o._bits;
  }

  bool operator!=(const FloatingPoint& o) const { return !(*this == o); }

  /// \brief Hash value
  std::size_t hash() const {
    return std::hash<uint64_t>()(this->_bits) ^
           (std::hash<uint16_t>()(this->_bit_width) << 1) ^
           (std::hash<uint8_t>()(static_cast<uint8_t>(this->_kind)) << 2);
  }

  /// \brief Dump for debugging
  void dump(std::ostream& o) const {
    if (this->_kind == Unmodeled) {
      o << "<unmodeled fp" << this->_bit_width << ">";
      return;
    }
    if (this->_bit_width == 32) {
      o << this->to_float() << "f";
    }
    else {
      o << this->to_double();
    }
  }

private:
  /// \brief Position of the most significant bit for this width
  static constexpr int msb_of(uint16_t bit_width) {
    return bit_width == 32 ? 31 : 63;
  }

  /// \brief True if every bit except the sign bit is zero, i.e. +/-0.0
  static bool is_signed_zero_bits(const FloatingPoint& a) {
    if (a._bit_width == 32) {
      return (a._bits & 0x7fffffffULL) == 0;
    }
    return (a._bits & 0x7fffffffffffffffULL) == 0;
  }

  /// \brief Arithmetic requires both modeled and same width
  ///
  /// The checks compile away under NDEBUG, hence maybe_unused.
  static void require_compatible([[maybe_unused]] const FloatingPoint& a,
                                [[maybe_unused]] const FloatingPoint& b) {
    ikos_assert_msg(a.is_modeled() && b.is_modeled(),
                    "floating point arithmetic on an unmodeled width");
    ikos_assert_msg(a._bit_width == b._bit_width,
                    "floating point arithmetic on mismatched widths");
  }

  friend std::ostream& operator<<(std::ostream& o, const FloatingPoint& f) {
    f.dump(o);
    return o;
  }
};

} // end namespace core
} // end namespace ikos

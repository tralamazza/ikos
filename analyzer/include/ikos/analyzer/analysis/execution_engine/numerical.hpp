/******************************************************************************
 *
 * \file
 * \brief Numerical execution engine
 *
 * Author: Maxime Arthaud
 *
 * Contributors: Jorge A. Navas
 *               Clement Decoodt
 *               Thomas Bailleux
 *
 * Contact: ikos@lists.nasa.gov
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
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>

#include <ikos/ar/semantic/intrinsic.hpp>
#include <ikos/ar/verify/type.hpp>

#include <ikos/core/domain/exception/abstract_domain.hpp>
#include <ikos/core/domain/memory/abstract_domain.hpp>
#include <ikos/core/domain/scalar/float_interval.hpp>

#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/engine.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/liveness.hpp>
#include <ikos/analyzer/analysis/option.hpp>
#include <ikos/analyzer/analysis/pointer/value.hpp>
#include <ikos/analyzer/support/assert.hpp>
#include <ikos/analyzer/support/cast.hpp>

namespace ikos {
namespace analyzer {

/// \brief Numerical execution engine
///
/// This class performs the transfer function on each AR (Abstract
/// Representation) statement with different levels of precision.
///
/// It relies on an abstract domain.
///
/// The abstract domain must implement the exception abstract domain interface
/// to handle exception propagation correctly.
///
/// The exception abstract domain must provide an underlying abstract domain
/// that must implement the memory abstract domain interface to handle scalar
/// variables and memory operations.
template < typename AbstractDomain >
class NumericalExecutionEngine final : public ExecutionEngine {
public:
  static_assert(core::exception::IsAbstractDomain< AbstractDomain >::value,
                "AbstractDomain must implement exception::AbstractDomain");
  static_assert(core::memory::IsAbstractDomain<
                    typename AbstractDomain::UnderlyingDomainT,
                    Variable*,
                    MemoryLocation* >::value,
                "AbstractDomain::UnderlyingDomainT must implement "
                "memory::AbstractDomain");

private:
  using IntInterval = core::machine_int::Interval;
  using IntIntervalCongruence = core::machine_int::IntervalCongruence;
  using IntVariable = core::VariableExpression< MachineInt, Variable* >;
  using IntLinearExpression = core::LinearExpression< MachineInt, Variable* >;
  using IntLinearConstraint = core::LinearConstraint< MachineInt, Variable* >;
  using IntLinearConstraintSystem =
      core::LinearConstraintSystem< MachineInt, Variable* >;
  using Nullity = core::Nullity;
  using Uninitialized = core::Uninitialized;
  using Lifetime = core::Lifetime;
  using IntUnaryOperator = core::machine_int::UnaryOperator;
  using IntBinaryOperator = core::machine_int::BinaryOperator;
  using IntPredicate = core::machine_int::Predicate;
  using PointerPredicate = core::pointer::Predicate;

private:
  /// \brief Current invariant
  AbstractDomain _inv;

  /// \brief Analysis context
  Context& _ctx;

  /// \brief Memory location factory
  MemoryFactory& _mem_factory;

  /// \brief Variable factory
  VariableFactory& _var_factory;

  /// \brief Literal factory
  LiteralFactory& _lit_factory;

  /// \brief Data layout
  const ar::DataLayout& _data_layout;

  /// \brief Call context
  CallContext* _call_context;

  /// \brief Execution engine options
  ExecutionEngineOptions _opts;

  /// \brief Optional liveness information
  const LivenessAnalysis* _liveness;

  /// \brief Optional pointer information
  const PointerInfo* _pointer_info;

public:
  /// \brief Constructor
  ///
  /// \param inv Initial invariant
  /// \param ctx Analysis context
  /// \param call_context Calling context
  /// \param opts Execution engine options
  /// \param liveness Liveness analysis, or null
  /// \param pointer_info Pointer information, or null
  NumericalExecutionEngine(AbstractDomain inv,
                           Context& ctx,
                           CallContext* call_context,
                           ExecutionEngineOptions opts,
                           const LivenessAnalysis* liveness = nullptr,
                           const PointerInfo* pointer_info = nullptr)
      : _inv(std::move(inv)),
        _ctx(ctx),
        _mem_factory(*ctx.mem_factory),
        _var_factory(*ctx.var_factory),
        _lit_factory(*ctx.lit_factory),
        _data_layout(ctx.bundle->data_layout()),
        _call_context(call_context),
        _opts(opts),
        _liveness(liveness),
        _pointer_info(pointer_info) {}

private:
  /// \brief Private copy constructor
  NumericalExecutionEngine(const NumericalExecutionEngine&) = default;

public:
  /// \brief Public move constructor
  NumericalExecutionEngine(NumericalExecutionEngine&&) noexcept(
      std::is_nothrow_move_constructible< AbstractDomain >::value) = default;

  /// \brief No copy assignment operator
  NumericalExecutionEngine& operator=(const NumericalExecutionEngine&) = delete;

  /// \brief No move assignment operator
  NumericalExecutionEngine& operator=(NumericalExecutionEngine&&) = delete;

  /// \brief Destructor
  ~NumericalExecutionEngine() override = default;

  /// \brief Create a fresh numerical execution engine, with its own abstract
  /// domain
  NumericalExecutionEngine fork() const { return *this; }

  /// \brief Return the current invariant
  AbstractDomain& inv() { return this->_inv; }

  /// \brief Return the current invariant
  const AbstractDomain& inv() const { return this->_inv; }

  /// \brief Update the current invariant
  void set_inv(const AbstractDomain& inv) { this->_inv = inv; }

  /// \brief Update the current invariant
  void set_inv(AbstractDomain&& inv) { this->_inv = std::move(inv); }

  /// \brief Return the liveness analysis used, or null
  const LivenessAnalysis* liveness() const { return this->_liveness; }

  /// \brief Return the pointer information, or null
  const PointerInfo* pointer_info() const { return this->_pointer_info; }

public:
  /// \name Helpers to allocate memory
  /// @{

  /// \brief Initial value for a memory block
  enum class MemoryInitialValue {
    // Memory is initialized with zeros
    Zero,

    // Memory is uninitialized, reading it is an error
    Uninitialized,

    // Memory is unknown, reading it returns a non-deterministic value
    Unknown,
  };

  /// \brief Allocate a new memory object `addr` with unknown size
  ///
  /// We consider as a memory object an alloca (i.e., stack variables), global
  /// variables, malloc-like allocation sites, function pointers, and
  /// destination of inttoptr instructions. Also, variables whose address might
  /// have been taken are translated to global variables by the front-end.
  void allocate_memory(Variable* ptr,
                       MemoryLocation* addr,
                       Nullity nullity,
                       Lifetime lifetime,
                       MemoryInitialValue init_val) {
    // Update pointer
    this->_inv.normal().pointer_assign(ptr, addr, nullity);

    // Update memory location lifetime
    this->_inv.normal().lifetime_set(addr, lifetime);

    // Update memory value
    if (init_val == MemoryInitialValue::Zero) {
      this->_inv.normal().mem_zero_reachable(ptr);
    } else if (init_val == MemoryInitialValue::Uninitialized) {
      this->_inv.normal().mem_uninitialize_reachable(ptr);
    } else if (init_val == MemoryInitialValue::Unknown) {
      this->_inv.normal().mem_forget_reachable(ptr);
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Allocate a new memory object `addr` of size `alloc_size` (in bytes)
  void allocate_memory(Variable* ptr,
                       MemoryLocation* addr,
                       Nullity nullity,
                       Lifetime lifetime,
                       MemoryInitialValue init_val,
                       const MachineInt& alloc_size) {
    // Update pointer, lifetime and initial value for the memory location
    this->allocate_memory(ptr, addr, nullity, lifetime, init_val);
    if (init_val == MemoryInitialValue::Uninitialized) {
      // When the size of the allocation is known (like it is here)
      // we mark the storage as uninitialized by assigning it
      // the undefined value.
      this->_inv.normal().mem_write(ptr, ScalarLit::undefined(), alloc_size);
    }

    if (this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
      // Update allocation size var
      Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
      this->_inv.normal().int_assign(alloc_size_var, alloc_size);
    }
  }

  /// \brief Allocate a new memory object `addr` of size `alloc_size` (in bytes)
  void allocate_memory(Variable* ptr,
                       MemoryLocation* addr,
                       Nullity nullity,
                       Lifetime lifetime,
                       MemoryInitialValue init_val,
                       Variable* alloc_size) {
    // Update pointer, lifetime and initial value for the memory location
    this->allocate_memory(ptr, addr, nullity, lifetime, init_val);

    if (this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
      // Update allocation size var
      Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
      this->_inv.normal().uninit_assert_initialized(alloc_size);
      this->_inv.normal().int_assign(alloc_size_var, alloc_size);
    }
  }

private:
  /// @}
  /// \name Internal helpers
  /// @{

  /// \brief Initialize a global variable or function pointer operand
  ///
  /// Global variables and constant function pointers are not stored in the
  /// initial invariant, so it is necessary to initialize them on the fly when
  /// we need them.
  void init_global_operand(ar::Value* value) {
    if (auto gv = dyn_cast< ar::GlobalVariable >(value)) {
      this->_inv.normal().pointer_assign(this->_var_factory.get_global(gv),
                                         this->_mem_factory.get_global(gv),
                                         Nullity::non_null());
    } else if (auto fun_ptr = dyn_cast< ar::FunctionPointerConstant >(value)) {
      auto fun = fun_ptr->function();
      this->_inv.normal().pointer_assign(this->_var_factory.get_function_ptr(
                                             fun),
                                         this->_mem_factory.get_function(fun),
                                         Nullity::non_null());
    } else if (auto struct_cst = dyn_cast< ar::StructConstant >(value)) {
      for (auto it = struct_cst->field_begin(), et = struct_cst->field_end();
           it != et;
           ++it) {
        this->init_global_operand(it->value);
      }
    } else if (auto seq_cst = dyn_cast< ar::SequentialConstant >(value)) {
      for (auto it = seq_cst->element_begin(), et = seq_cst->element_end();
           it != et;
           ++it) {
        this->init_global_operand(*it);
      }
    }
  }

  /// \brief Initialize global variables and function pointer operands
  void init_global_operands(ar::Statement* s) {
    for (auto it = s->op_begin(), et = s->op_end(); it != et; ++it) {
      this->init_global_operand(*it);
    }
  }

  /// \brief Prepare a memory access (read/write) on the given pointer
  ///
  /// Return true if the memory access can be performed, i.e the pointer is
  /// non-null and well defined
  bool prepare_mem_access(const ScalarLit& ptr) {
    if (ptr.is_undefined()) {
      // Undefined pointer dereference
      this->_inv.set_normal_flow_to_bottom();
      return false;
    } else if (ptr.is_null()) {
      // Null pointer dereference
      this->_inv.set_normal_flow_to_bottom();
      return false;
    }

    ikos_assert_msg(ptr.is_pointer_var(), "unexpected parameter");

    // Reduction between value and pointer analysis
    this->refine_addresses_offset(ptr.var());

    // Assert `ptr != null`
    this->_inv.normal().nullity_assert_non_null(ptr.var());

    this->_inv.normal().normalize();

    // Ready for read/write
    return !this->_inv.is_normal_flow_bottom();
  }

  /// \brief Normalize the nullity domain
  ///
  /// Check if the given pointer variable points to AbsoluteZeroMemoryLocation.
  /// If so, check if the offset interval contains zero, and update the nullity
  /// domain accordingly.
  void normalize_absolute_zero_nullity(Variable* p) {
    auto nullity = this->_inv.normal().nullity_to_nullity(p);
    if (nullity.is_bottom() || nullity.is_top()) {
      return;
    }

    PointsToSet addrs = this->_inv.normal().pointer_to_points_to(p);

    if (addrs.contains(this->_mem_factory.get_absolute_zero())) {
      IntIntervalCongruence offset =
          this->_inv.normal().pointer_offset_to_interval_congruence(p);
      auto zero = MachineInt::zero(offset.bit_width(), offset.sign());

      if (offset.is_bottom()) {
        return;
      } else if (addrs.singleton()) {
        if (offset.singleton() == boost::optional< MachineInt >(zero)) {
          // Pointer is definitely null (base is zero, offset = 0)
          this->_inv.normal().nullity_set(p, Nullity::null());
        } else if (!offset.contains(zero)) {
          // Pointer is definitely non-null (base is zero, offset != 0)
          this->_inv.normal().nullity_set(p, Nullity::non_null());
        } else {
          // Pointer might be null (base is zero, offset contains zero)
          this->_inv.normal().nullity_set(p, Nullity::top());
        }
      } else if (offset.contains(zero)) {
        // Pointer might be null (base might be zero, offset contains zero)
        this->_inv.normal().nullity_set(p, Nullity::top());
      }
    }
  }

  /// \brief Refine the addresses of `ptr` using information from an external
  /// pointer analysis
  void refine_addresses(Variable* ptr) {
    if (!this->_pointer_info) {
      return;
    }

    PointerAbsValue value = this->_pointer_info->get(ptr);
    this->_inv.normal().pointer_refine(ptr, value.points_to());
  }

  /// \brief Refine the addresses and offset of `ptr` using information from an
  /// external pointer analysis
  void refine_addresses_offset(Variable* ptr) {
    if (!this->_pointer_info) {
      return;
    }

    PointerAbsValue value = this->_pointer_info->get(ptr);
    this->_inv.normal().pointer_refine(ptr, value);
  }

private:
  /// @}
  /// \name Helpers for assignments
  /// @{

  /// \brief Integer variable assignment
  class IntegerAssign : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    IntegerAssign(Variable* lhs, AbstractDomain& inv) : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt& rhs) {
      this->_inv.normal().int_assign(this->_lhs, rhs);
    }

    void floating_point(const FloatingPoint&) { ikos_unreachable("unreachable"); }

    void memory_location(MemoryLocation*) { ikos_unreachable("unreachable"); }

    void null() { ikos_unreachable("unreachable"); }

    void undefined() { this->_inv.normal().int_assign_undef(this->_lhs); }

    void machine_int_var(Variable* rhs) {
      this->_inv.normal().int_assign(this->_lhs, rhs);
    }

    void floating_point_var(Variable*) { ikos_unreachable("unreachable"); }

    void pointer_var(Variable*) { ikos_unreachable("unreachable"); }

  }; // end class IntegerAssign

  /// \brief Floating point variable assignment
  class FloatingPointAssign : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    FloatingPointAssign(Variable* lhs, AbstractDomain& inv)
        : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt&) { ikos_unreachable("unreachable"); }

    // Assigning a known FP constant must record that constant. Topping it here
    // discarded the value, so a plain `double c = 0.0;` left `c` unknown in the
    // FP interval domain and any later join saw top instead of [0, 0].
    void floating_point(const FloatingPoint& cst) {
      this->_inv.normal().float_assign_cst(this->_lhs, cst);
    }

    void memory_location(MemoryLocation*) { ikos_unreachable("unreachable"); }

    void null() { ikos_unreachable("unreachable"); }

    void undefined() { this->_inv.normal().float_assign_undef(this->_lhs); }

    void machine_int_var(Variable*) { ikos_unreachable("unreachable"); }

    void floating_point_var(Variable* rhs) {
      this->_inv.normal().float_assign(this->_lhs, rhs);
    }

    void pointer_var(Variable*) { ikos_unreachable("unreachable"); }

  }; // end class FloatingPointAssign

  /// \brief Pointer variable assignment
  class PointerAssign : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    PointerAssign(Variable* lhs, AbstractDomain& inv) : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt&) { ikos_unreachable("unreachable"); }

    void floating_point(const FloatingPoint&) { ikos_unreachable("unreachable"); }

    void memory_location(MemoryLocation* addr) {
      this->_inv.normal().pointer_assign(this->_lhs, addr, Nullity::non_null());
    }

    void null() { this->_inv.normal().pointer_assign_null(this->_lhs); }

    void undefined() { this->_inv.normal().pointer_assign_undef(this->_lhs); }

    void machine_int_var(Variable*) { ikos_unreachable("unreachable"); }

    void floating_point_var(Variable*) { ikos_unreachable("unreachable"); }

    void pointer_var(Variable* rhs) {
      this->_inv.normal().pointer_assign(this->_lhs, rhs);
    }

  }; // end class PointerAssign

  /// \brief Scalar assignment `lhs = rhs`
  ///
  /// Requires that `lhs` and `rhs` have the same type.
  /// Propagates uninitialized variables.
  void assign(const ScalarLit& lhs, const ScalarLit& rhs) {
    if (lhs.is_machine_int_var()) {
      IntegerAssign v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else if (lhs.is_floating_point_var()) {
      FloatingPointAssign v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else if (lhs.is_pointer_var()) {
      PointerAssign v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else {
      ikos_unreachable("left hand side is not a variable");
    }
  }

private:
  /// @}
  /// \name Helpers for implicit bitcasts
  /// @{

  /// \brief Integer variable implicit bitcast
  class IntegerImplicitBitcast : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    ar::IntegerType* _type;
    AbstractDomain& _inv;

  public:
    IntegerImplicitBitcast(Variable* lhs, AbstractDomain& inv)
        : _lhs(lhs),
          _type(ar::cast< ar::IntegerType >(lhs->type())),
          _inv(inv) {}

    void machine_int(const MachineInt& rhs) {
      ikos_assert(this->_type->bit_width() == rhs.bit_width());
      if (this->_type->sign() == rhs.sign()) {
        this->_inv.normal().int_assign(this->_lhs, rhs);
      } else {
        this->_inv.normal().int_assign(this->_lhs,
                                       rhs.sign_cast(this->_type->sign()));
      }
    }

    void floating_point(const FloatingPoint&) { ikos_unreachable("unreachable"); }

    void memory_location(MemoryLocation*) { ikos_unreachable("unreachable"); }

    void null() { ikos_unreachable("unreachable"); }

    void undefined() { this->_inv.set_normal_flow_to_bottom(); }

    void machine_int_var(Variable* rhs) {
      auto rhs_type = ar::cast< ar::IntegerType >(rhs->type());

      ikos_assert(this->_type->bit_width() == rhs_type->bit_width());
      if (this->_type->sign() == rhs_type->sign()) {
        this->_inv.normal().uninit_assert_initialized(rhs);
        this->_inv.normal().int_assign(this->_lhs, rhs);
      } else {
        this->_inv.normal().int_apply(IntUnaryOperator::SignCast,
                                      this->_lhs,
                                      rhs);
      }
    }

    void floating_point_var(Variable*) { ikos_unreachable("unreachable"); }

    void pointer_var(Variable*) { ikos_unreachable("unreachable"); }

  }; // end class IntegerImplicitBitcast

  /// \brief Floating point variable implement bitcast
  class FloatingPointImplicitBitcast : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    FloatingPointImplicitBitcast(Variable* lhs, AbstractDomain& inv)
        : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt&) { ikos_unreachable("unreachable"); }

    // Same as in FloatingPointAssign: keep the constant rather than topping it.
    void floating_point(const FloatingPoint& cst) {
      this->_inv.normal().float_assign_cst(this->_lhs, cst);
    }

    void memory_location(MemoryLocation*) { ikos_unreachable("unreachable"); }

    void null() { ikos_unreachable("unreachable"); }

    void undefined() { this->_inv.set_normal_flow_to_bottom(); }

    void machine_int_var(Variable*) { ikos_unreachable("unreachable"); }

    void floating_point_var(Variable* rhs) {
      this->_inv.normal().uninit_assert_initialized(rhs);
      this->_inv.normal().float_assign(this->_lhs, rhs);
    }

    void pointer_var(Variable*) { ikos_unreachable("unreachable"); }

  }; // end class FloatingPointImplicitBitcast

  /// \brief Pointer variable implicit bitcast
  class PointerImplicitBitcast : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    PointerImplicitBitcast(Variable* lhs, AbstractDomain& inv)
        : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt&) { ikos_unreachable("unreachable"); }

    void floating_point(const FloatingPoint&) { ikos_unreachable("unreachable"); }

    void memory_location(MemoryLocation* addr) {
      this->_inv.normal().pointer_assign(this->_lhs, addr, Nullity::non_null());
    }

    void null() { this->_inv.normal().pointer_assign_null(this->_lhs); }

    void undefined() { this->_inv.set_normal_flow_to_bottom(); }

    void machine_int_var(Variable*) { ikos_unreachable("unreachable"); }

    void floating_point_var(Variable*) { ikos_unreachable("unreachable"); }

    void pointer_var(Variable* rhs) {
      this->_inv.normal().uninit_assert_initialized(rhs);
      this->_inv.normal().pointer_assign(this->_lhs, rhs);
    }

  }; // end class PointerImplicitBitcast

  /// \brief Implicit bitcast `lhs = rhs`
  ///
  /// Requires either one of:
  ///  - `lhs` and `rhs` have the same type
  ///  - `lhs` and `rhs` are integers of same bit-width (signed <-> unsigned)
  ///  - `lhs` and `rhs` are pointer types (ie., A* <-> B*)
  ///
  /// Implicit bitcast on an uninitialized variable is an error.
  void implicit_bitcast(const ScalarLit& lhs, const ScalarLit& rhs) {
    if (lhs.is_machine_int_var()) {
      IntegerImplicitBitcast v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else if (lhs.is_floating_point_var()) {
      FloatingPointImplicitBitcast v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else if (lhs.is_pointer_var()) {
      PointerImplicitBitcast v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else {
      ikos_unreachable("left hand side is not a variable");
    }
  }

private:
  /// @}
  /// \name Helpers for aggregate (struct, array) statements
  /// @{

  /// \brief Return the type void*
  ar::Type* void_ptr_type() const {
    ar::Context& ctx = _ctx.bundle->context();
    return ar::PointerType::get(ctx, ar::VoidType::get(ctx));
  }

  /// \brief Initialize an aggregate memory location
  ///
  /// Internal variables of aggregate types are modeled as if they were in
  /// memory, at a symbolic location.
  ///
  /// Returns a pointer to the symbolic location of the aggregate in memory.
  Variable* init_aggregate_memory(const AggregateLit& aggregate) {
    ikos_assert_msg(aggregate.is_var(), "aggregate is not a variable");

    auto var = cast< InternalVariable >(aggregate.var());
    this->allocate_memory(var,
                          this->_mem_factory.get_aggregate(var->internal_var()),
                          Nullity::non_null(),
                          Lifetime::top(),
                          MemoryInitialValue::Zero);
    return var;
  }

  /// \brief Return a pointer to the symbolic location of the aggregate in
  /// memory
  Variable* aggregate_pointer(const AggregateLit& aggregate) {
    ikos_assert_msg(aggregate.is_var(), "aggregate is not a variable");

    auto var = cast< InternalVariable >(aggregate.var());
    this->_inv.normal().pointer_assign(var,
                                       this->_mem_factory.get_aggregate(
                                           var->internal_var()),
                                       Nullity::non_null());
    return var;
  }

  /// \brief Write an aggregate in the memory
  void mem_write_aggregate(Variable* ptr, const AggregateLit& aggregate) {
    if (aggregate.size().is_zero()) {
      return; // Nothing to do
    } else if (aggregate.is_cst()) {
      // Pointer to write the aggregate in the memory
      Variable* write_ptr =
          this->_var_factory.get_named_shadow(this->void_ptr_type(),
                                              "shadow.mem_write_aggregate.ptr");

      for (const auto& field : aggregate.fields()) {
        this->_inv.normal().pointer_assign(write_ptr, ptr, field.offset);
        this->_inv.normal().mem_write(write_ptr, field.value, field.size);
      }

      // Clean-up
      this->_inv.normal().pointer_forget(write_ptr);
    } else if (aggregate.is_zero() || aggregate.is_undefined()) {
      // aggregate.size() is in bytes, compute bit-width, and check
      // if the bit-width fits in an unsigned int
      // XXX: We should use mem_zero_reachable and mem_uninitialize_reachable
      bool overflow;
      MachineInt eight(8, aggregate.size().bit_width(), Unsigned);
      MachineInt bit_width = mul(aggregate.size(), eight, overflow);
      if (overflow || !bit_width.fits< uint64_t >()) {
        // Too big for a cell
        this->_inv.normal().mem_forget_reachable(ptr);
      } else if (aggregate.is_zero()) {
        MachineInt zero(0, bit_width.to< uint64_t >(), Signed);
        this->_inv.normal().mem_write(ptr,
                                      ScalarLit::machine_int(zero),
                                      aggregate.size());
      } else if (aggregate.is_undefined()) {
        this->_inv.normal().mem_write(ptr,
                                      ScalarLit::undefined(),
                                      aggregate.size());
      } else {
        ikos_unreachable("unreachable");
      }
    } else if (aggregate.is_var()) {
      Variable* aggregate_ptr = this->aggregate_pointer(aggregate);
      this->_inv.normal().mem_copy(ptr,
                                   aggregate_ptr,
                                   ScalarLit::machine_int(aggregate.size()));
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Aggregate assignment `lhs = rhs`
  ///
  /// Propagates uninitialized variables.
  void assign(const AggregateLit& lhs, const AggregateLit& rhs) {
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    Variable* ptr = this->init_aggregate_memory(lhs);
    this->mem_write_aggregate(ptr, rhs);
  }

  /// \brief Assignment `lhs = rhs`
  ///
  /// Requires that `lhs` and `rhs` have the same type.
  /// Propagates uninitialized variables.
  void assign(const Literal& lhs, const Literal& rhs) {
    if (lhs.is_scalar()) {
      ikos_assert_msg(rhs.is_scalar(), "unexpected right hand side");
      this->assign(lhs.scalar(), rhs.scalar());
    } else if (lhs.is_aggregate()) {
      ikos_assert_msg(rhs.is_aggregate(), "unexpected right hand side");
      this->assign(lhs.aggregate(), rhs.aggregate());
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Implicit bitcast `lhs = rhs`
  ///
  /// Requires either one of:
  ///  - `lhs` and `rhs` have the same type
  ///  - `lhs` and `rhs` are integers of same bit-width (signed <-> unsigned)
  ///  - `lhs` and `rhs` are pointer types (ie., A* <-> B*)
  ///
  /// Implicit bitcast on an uninitialized variable is an error.
  void implicit_bitcast(const Literal& lhs, const Literal& rhs) {
    if (lhs.is_scalar()) {
      ikos_assert_msg(rhs.is_scalar(), "unexpected right hand side");
      this->implicit_bitcast(lhs.scalar(), rhs.scalar());
    } else if (lhs.is_aggregate()) {
      ikos_assert_msg(rhs.is_aggregate(), "unexpected right hand side");
      this->assign(lhs.aggregate(), rhs.aggregate());
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Randomly throw unknown exceptions with the current invariant
  ///
  /// Equivalent to if (rand()) { throw rand(); }
  void throw_unknown_exceptions() {
    this->_inv.caught_exceptions().join_with(this->_inv.normal());
  }

public:
  /// \brief Deallocate the memory for the given local variables
  void deallocate_local_variables(ar::Function::LocalVariableIterator begin,
                                  ar::Function::LocalVariableIterator end) {
    for (auto it = begin; it != end; ++it) {
      LocalVariable* var = this->_var_factory.get_local(*it);
      MemoryLocation* addr = this->_mem_factory.get_local(*it);

      // Forget local variable pointer
      this->_inv.normal().pointer_forget(var);
      this->_inv.caught_exceptions().pointer_forget(var);
      this->_inv.propagated_exceptions().pointer_forget(var);

      // Forget the memory content
      this->_inv.normal().mem_forget(addr);
      this->_inv.caught_exceptions().mem_forget(addr);
      this->_inv.propagated_exceptions().mem_forget(addr);

      // Set the memory location lifetime to deallocated
      this->_inv.normal().lifetime_assign_deallocated(addr);
      this->_inv.caught_exceptions().lifetime_assign_deallocated(addr);
      this->_inv.propagated_exceptions().lifetime_assign_deallocated(addr);

      if (this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
        // Forget the allocation size variable
        AllocSizeVariable* alloc_size_var =
            this->_var_factory.get_alloc_size(addr);
        this->_inv.normal().int_forget(alloc_size_var);
        this->_inv.caught_exceptions().int_forget(alloc_size_var);
        this->_inv.propagated_exceptions().int_forget(alloc_size_var);
      }
    }
  }

public:
  /// @}
  /// \name Implement ExecutionEngine
  /// @{

  /// \brief Enter a basic block
  void exec_enter(ar::BasicBlock*) override {}

  /// \brief Leave a basic block
  ///
  /// Use the liveness analysis to remove dead variables
  void exec_leave(ar::BasicBlock* bb) override {
    if (this->_liveness == nullptr) {
      return;
    }

    boost::optional< const LivenessAnalysis::VariableRefList& > dead =
        this->_liveness->dead_at_end(bb);

    if (!dead) {
      return;
    }

    // Do not remove the returned variable
    Variable* returned_var = nullptr;
    if (!bb->empty() && isa< ar::ReturnValue >(bb->back())) {
      auto ret = cast< ar::ReturnValue >(bb->back());

      if (ret->has_operand()) {
        const Literal& v = this->_lit_factory.get(ret->operand());

        if (v.is_var()) {
          returned_var = v.var();
        }
      }
    }

    for (Variable* var : *dead) {
      if (var == returned_var) { // Ignore
        continue;
      }

      // Special case for aggregate internal variables: Clean-up the memory
      if (auto iv = dyn_cast< InternalVariable >(var)) {
        ar::InternalVariable* ar_iv = iv->internal_var();
        if (ar_iv->type()->is_aggregate()) {
          MemoryLocation* addr = this->_mem_factory.get_aggregate(ar_iv);
          this->_inv.normal().mem_forget(addr);
          this->_inv.caught_exceptions().mem_forget(addr);
          this->_inv.propagated_exceptions().mem_forget(addr);
        }
      }

      // Clean-up scalars
      this->_inv.normal().scalar_forget(var);
      this->_inv.caught_exceptions().scalar_forget(var);
      this->_inv.propagated_exceptions().scalar_forget(var);
    }
  }

  /// \brief Execute an edge from `src` to `dest`
  void exec_edge(ar::BasicBlock* src, ar::BasicBlock* dest) override {
    // Check if the source block ends with an invoke

    if (src->empty()) {
      return;
    }

    ar::Statement* stmt = src->back();
    if (!isa< ar::Invoke >(stmt)) {
      return;
    }

    auto invoke = cast< ar::Invoke >(stmt);
    if (invoke->normal_dest() == dest) {
      this->_inv.enter_normal();
    } else if (invoke->exception_dest() == dest) {
      this->_inv.enter_catch();
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute an Assignment statement
  ///
  /// Unlike most statements, this propagates uninitialized variables.
  void exec(ar::Assignment* s) override {
    this->init_global_operands(s);

    this->assign(this->_lit_factory.get(s->result()),
                 this->_lit_factory.get(s->operand()));
  }

  /// \brief Execute an UnaryOperation statement
  void exec(ar::UnaryOperation* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const Literal& lhs = this->_lit_factory.get(s->result());
    const Literal& rhs = this->_lit_factory.get(s->operand());

    switch (s->op()) {
      case ar::UnaryOperation::UTrunc:
      case ar::UnaryOperation::STrunc: {
        this->exec_int_conv(IntUnaryOperator::Trunc,
                            lhs.scalar(),
                            rhs.scalar());
      } break;
      case ar::UnaryOperation::ZExt:
      case ar::UnaryOperation::SExt: {
        this->exec_int_conv(IntUnaryOperator::Ext, lhs.scalar(), rhs.scalar());
      } break;
      case ar::UnaryOperation::FPTrunc: {
        this->exec_float_conv(lhs.scalar(), rhs.scalar());
      } break;
      case ar::UnaryOperation::FPExt: {
        this->exec_float_conv(lhs.scalar(), rhs.scalar());
      } break;
      case ar::UnaryOperation::FPToUI: {
        this->exec_float_to_int_conv(lhs.scalar(), rhs.scalar(), false);
      } break;
      case ar::UnaryOperation::FPToSI: {
        this->exec_float_to_int_conv(lhs.scalar(), rhs.scalar(), true);
      } break;
      case ar::UnaryOperation::UIToFP: {
        this->exec_int_to_float_conv(lhs.scalar(), rhs.scalar(), false);
      } break;
      case ar::UnaryOperation::SIToFP: {
        this->exec_int_to_float_conv(lhs.scalar(), rhs.scalar(), true);
      } break;
      case ar::UnaryOperation::PtrToUI:
      case ar::UnaryOperation::PtrToSI: {
        this->exec_ptr_to_int_conv(lhs.scalar(), rhs.scalar());
      } break;
      case ar::UnaryOperation::UIToPtr:
      case ar::UnaryOperation::SIToPtr: {
        this->exec_int_to_ptr_conv(lhs.scalar(), rhs.scalar());
      } break;
      case ar::UnaryOperation::Bitcast: {
        this->exec_bitcast(s, lhs, rhs);
      } break;
    }
  }

private:
  /// \brief Execute an integer conversion
  void exec_int_conv(IntUnaryOperator op,
                     const ScalarLit& lhs,
                     const ScalarLit& rhs) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (rhs.is_machine_int()) {
      auto type = cast< ar::IntegerType >(lhs.var()->type());
      this->_inv.normal()
          .int_assign(lhs.var(),
                      core::machine_int::apply_unary_operator(op,
                                                              rhs.machine_int(),
                                                              type->bit_width(),
                                                              type->sign()));
    } else if (rhs.is_machine_int_var()) {
      this->_inv.normal().int_apply(op, lhs.var(), rhs.var());
    } else {
      ikos_unreachable("unexpected arguments");
    }
  }

  /// \brief Round `v` into width `bits`, forcing the result outward.
  ///
  /// The C++ cast rounds to nearest, which can land on the far side of `v` and
  /// so exclude a value the program can actually produce. Casting the result
  /// back to double is exact (the target is narrower), so the direction the cast
  /// went is visible and one step of nextafter corrects it.
  ///
  /// Overflow needs no special case: a value beyond the finite range casts to
  /// infinity, and the correction brings it back to the largest finite number
  /// when rounding down while leaving it at infinity going up. Both are the
  /// sound outward answer.
  static FloatingPoint round_outward(double v, uint16_t bits, bool up) {
    if (bits == 32) {
      float f = static_cast< float >(v);
      double back = static_cast< double >(f);  // exact: float is narrower
      if (up && back < v) {
        f = std::nextafterf(f, std::numeric_limits< float >::infinity());
      } else if (!up && back > v) {
        f = std::nextafterf(f, -std::numeric_limits< float >::infinity());
      }
      return FloatingPoint::from_float(f);
    }
    if (bits == 64) {
      // binary64 is the widest width we model, so narrowing into it is exact.
      return FloatingPoint::from_double(v);
    }
    return FloatingPoint::unmodeled(bits);
  }

  /// \brief Execute a floating point to floating point conversion
  ///
  /// Covers both fpext (exact) and fptrunc (rounds). Constants are reinterpreted
  /// into the destination width; for non-constants the interval is carried
  /// across, outward-rounded when narrowing.
  void exec_float_conv(const ScalarLit& lhs, const ScalarLit& rhs) {
    ikos_assert_msg(lhs.is_floating_point_var(),
                    "left hand side is not a floating point variable");

    if (rhs.is_floating_point_var()) {
      this->_inv.normal().uninit_assert_initialized(rhs.var());
    }

    const FloatingPoint* v = this->fp_value(rhs);
    if (v == nullptr) {
      // Not a constant. Widening is exact -- every binary32 value is a binary64
      // value -- so a known interval maps straight across with no rounding to
      // model. Narrowing rounds, so it keeps topping out rather than pretending.
      auto dst = ar::cast< ar::FloatType >(lhs.var()->type());
      auto src = rhs.is_floating_point_var()
                     ? ar::cast< ar::FloatType >(rhs.var()->type())
                     : nullptr;
      if (dst != nullptr && src != nullptr) {
        const core::floating_point::Interval* iv =
            this->_inv.normal().float_get_interval(rhs.var());
        if (iv != nullptr && !iv->is_empty()) {
          auto w = static_cast< uint16_t >(dst->bit_width());
          if (dst->bit_width() > src->bit_width()) {
            FloatingPoint lo = iv->lo.cast_width(w);
            FloatingPoint hi = iv->hi.cast_width(w);
            if (lo.is_modeled() && hi.is_modeled()) {
              this->_inv.normal().float_assign_interval(
                  lhs.var(), core::floating_point::Interval{lo, hi, iv->may_nan});
              return;
            }
          } else if (dst->bit_width() < src->bit_width() && !iv->may_nan) {
            // Narrowing rounds, so the bounds must go OUTWARD: lower down, upper
            // up. Rounding to nearest could clip a reachable value.
            FloatingPoint lo = round_outward(iv->lo.to_double(), w, false);
            FloatingPoint hi = round_outward(iv->hi.to_double(), w, true);
            if (lo.is_modeled() && hi.is_modeled()) {
              this->_inv.normal().float_assign_interval(
                  lhs.var(), core::floating_point::Interval{lo, hi, false});
              return;
            }
          }
        }
      }
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }

    auto type = ar::cast< ar::FloatType >(lhs.var()->type());
    FloatingPoint res = v->cast_width(
        static_cast<uint16_t>(type->bit_width())
    );
    if (!res.is_modeled()) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }
    this->_inv.normal().float_assign_cst(lhs.var(), res);
  }

  /// \brief Map a known floating point interval across a float-to-int cast.
  ///
  /// Truncation toward zero is monotonically non-decreasing -- trunc(-3.5) is
  /// -4, trunc(-0.5) is 0, trunc(2.5) is 2 -- so the image of [lo, hi] is
  /// exactly the contiguous integer set [trunc(lo), trunc(hi)]. That makes the
  /// mapping exact rather than an over-approximation, and means there is no
  /// directed-rounding argument to make here. The only thing that can go wrong is
  /// the image not fitting the target integer type, or NaN/Inf being involved,
  /// and both are refused.
  ///
  /// Returns false when nothing usable is known, leaving the caller to top out.
  bool fp_interval_to_int(Variable* lhs, const ScalarLit& rhs, bool is_signed) {
    if (!rhs.is_floating_point_var()) {
      return false;
    }
    const core::floating_point::Interval* iv =
        this->_inv.normal().float_get_interval(rhs.var());
    if (iv == nullptr || iv->is_empty() || iv->may_nan) {
      return false;
    }
    double lo = iv->lo.to_double();
    double hi = iv->hi.to_double();
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
      return false;
    }
    // fptoui of a negative value is undefined in C: there is no integer image
    // to report, so refuse rather than pick one.
    if (!is_signed && lo < 0.0) {
      return false;
    }

    auto type = ar::cast< ar::IntegerType >(lhs->type());
    if (type == nullptr) {
      return false;
    }
    int bits = static_cast<int>(type->bit_width());

    // Same exact-power-of-two convention as the constant path below: a double
    // can never equal 2^k - 1 for large k, so a strict upper bound never admits
    // an out-of-range value.
    double lim = std::ldexp(1.0, bits - (is_signed ? 1 : 0));
    double lim_lo = is_signed ? -lim : 0.0;
    double tlo = std::trunc(lo);
    double thi = std::trunc(hi);
    if (!(tlo >= lim_lo && thi < lim)) {
      return false;
    }

    auto w = static_cast< uint64_t >(bits);
    MachineInt ilo = is_signed
        ? MachineInt(static_cast<int64_t>(tlo), w, core::Signedness::Signed)
        : MachineInt(static_cast<uint64_t>(tlo), w, core::Signedness::Unsigned);
    MachineInt ihi = is_signed
        ? MachineInt(static_cast<int64_t>(thi), w, core::Signedness::Signed)
        : MachineInt(static_cast<uint64_t>(thi), w, core::Signedness::Unsigned);
    this->_inv.normal().int_set(lhs, IntInterval(ilo, ihi));
    return true;
  }

  /// \brief Execute a conversion from floating point to integer
  ///
  /// fptosi/fptoui truncate toward zero. A value outside the target integer's
  /// range is undefined behaviour in C, and NaN/Inf have no integer image at all,
  /// so those cases top out instead of picking a value.
  void exec_float_to_int_conv(const ScalarLit& lhs,
                              const ScalarLit& rhs,
                              bool is_signed) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (rhs.is_floating_point_var()) {
      this->_inv.normal().uninit_assert_initialized(rhs.var());
    }

    const FloatingPoint* v = this->fp_value(rhs);
    if (v == nullptr) {
      // Not a constant, but a known FP interval still pins the result exactly.
      if (!this->fp_interval_to_int(lhs.var(), rhs, is_signed)) {
        this->_inv.normal().int_assign_nondet(lhs.var());
      }
      return;
    }

    auto type = ar::cast< ar::IntegerType >(lhs.var()->type());
    int bits = static_cast<int>(type->bit_width());

    double t = v->trunc_toward_zero();
    if (std::isnan(t)) {
      this->_inv.normal().int_assign_nondet(lhs.var());
      return;
    }

    // Compare against exact powers of two. A double can never equal 2^k - 1 for
    // large k, so a strict upper bound never admits an out-of-range value.
    double hi = is_signed ? std::ldexp(1.0, bits - 1) : std::ldexp(1.0, bits);
    double lo = is_signed ? -hi : 0.0;
    if (!(t >= lo && t < hi)) {
      this->_inv.normal().int_assign_nondet(lhs.var());
      return;
    }

    MachineInt res = is_signed
        ? MachineInt(static_cast<int64_t>(t),
                    static_cast<uint64_t>(bits),
                    core::Signedness::Signed)
        : MachineInt(static_cast<uint64_t>(t),
                     static_cast<uint64_t>(bits),
                     core::Signedness::Unsigned);
    this->_inv.normal().int_assign(lhs.var(), res);
  }

  /// \brief Map a known integer interval across an int-to-float cast.
  ///
  /// Every integer of magnitude at most 2^p is exactly representable in a format
  /// with a (p+1)-bit significand: binary32 is exact up to 2^24, binary64 up to
  /// 2^53. Inside that range the cast is lossless and order-preserving, so the
  /// image of [a, b] is exactly [(double)a, (double)b]. That is what makes this
  /// sound with no directed-rounding argument at all -- there is no rounding.
  ///
  /// Past the threshold the cast rounds, so we refuse rather than try to model
  /// round-toward-zero lower bounds and round-toward-infinity upper bounds here.
  ///
  /// Returns false when nothing usable is known, leaving the caller to top out.
  bool int_interval_to_float(Variable* lhs, const ScalarLit& rhs,
                            bool is_signed) {
    if (!rhs.is_machine_int_var()) {
      return false;
    }
    auto type = ar::cast< ar::FloatType >(lhs->type());
    if (type == nullptr) {
      return false;
    }

    // Exactness threshold as a wide integer, so the comparison below cannot
    // overflow any machine width.
    uint64_t bits = type->bit_width();
    uint64_t shift = 0;
    if (bits == 32) {
      shift = 24;
    } else if (bits == 64) {
      shift = 53;
    } else {
      return false;
    }
    core::ZNumber limit = core::ZNumber(1) << static_cast< unsigned long int >(shift);

    IntInterval iv = this->_inv.normal().int_to_interval(rhs.var());
    if (iv.is_bottom()) {
      return false;
    }

    core::ZNumber lo = iv.lb().to_z_number();
    core::ZNumber hi = iv.ub().to_z_number();

    if (!is_signed && lo < 0) {
      // uitofp of something the signed interval calls negative: the unsigned
      // image is a huge positive value we are not modelling here.
      return false;
    }
    if (hi > limit || lo < -limit) {
      return false;
    }

    // ZNumber has no to_double(), and going through the decimal string plus
    // strtod is exact for integers this small -- strtod rounds correctly and
    // every value here is representable, so no rounding occurs.
    std::string slo = lo.str(10);
    std::string shi = hi.str(10);
    char* e1 = nullptr;
    char* e2 = nullptr;
    double dlo = std::strtod(slo.c_str(), &e1);
    double dhi = std::strtod(shi.c_str(), &e2);
    if (e1 != slo.c_str() + slo.size() || e2 != shi.c_str() + shi.size()) {
      return false;
    }

    FloatingPoint flo = (bits == 32)
                           ? FloatingPoint::from_float(static_cast< float >(dlo))
                           : FloatingPoint::from_double(dlo);
    FloatingPoint fhi = (bits == 32)
                           ? FloatingPoint::from_float(static_cast< float >(dhi))
                           : FloatingPoint::from_double(dhi);
    if (!flo.is_modeled() || !fhi.is_modeled()) {
      return false;
    }

    // An integer cast never produces a NaN.
    this->_inv.normal().float_assign_interval(
        lhs, core::floating_point::Interval{flo, fhi, false});
    return true;
  }

  /// \brief Execute a conversion from a machine integer to a floating point
  ///
  /// sitofp/uitofp are well defined for every input but can lose precision. Going
  /// through the exact decimal representation and a correctly rounded strtod/strtof
  /// keeps the result exact-to-the-width rather than double-rounding.
  void exec_int_to_float_conv(const ScalarLit& lhs,
                              const ScalarLit& rhs,
                              bool is_signed) {
    ikos_assert_msg(lhs.is_floating_point_var(),
                    "left hand side is not a floating point variable");

    if (rhs.is_machine_int_var()) {
      this->_inv.normal().uninit_assert_initialized(rhs.var());
    }

    const MachineInt* m = nullptr;
    if (rhs.is_machine_int()) {
      m = &rhs.machine_int();
    }
    if (m == nullptr) {
      // The operand is a variable. Its integer interval may still pin the
      // result exactly, so try that before giving up.
      if (!this->int_interval_to_float(lhs.var(), rhs, is_signed)) {
        this->_inv.normal().float_assign_nondet(lhs.var());
      }
      return;
    }

    // uitofp must read the operand's unsigned image. If the literal we hold is
    // negative we cannot tell whether that was the intent, so refuse to guess.
    std::string text = m->str(10);
    if (!is_signed && text.compare(0, 1, "-") == 0) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }

    auto type = ar::cast< ar::FloatType >(lhs.var()->type());
    uint16_t bits = static_cast<uint16_t>(type->bit_width());
    char* end = nullptr;
    FloatingPoint res = FloatingPoint::unmodeled(bits);
    if (bits == 32) {
      res = FloatingPoint::from_float(std::strtof(text.c_str(), &end));
    } else if (bits == 64) {
      res = FloatingPoint::from_double(std::strtod(text.c_str(), &end));
    }
    if (!res.is_modeled() || end != text.c_str() + text.size()) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }
    this->_inv.normal().float_assign_cst(lhs.var(), res);
  }

  /// \brief Execute a conversion from pointer to integer
  void exec_ptr_to_int_conv(const ScalarLit& lhs, const ScalarLit& rhs) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (rhs.is_null()) {
      auto type = cast< ar::IntegerType >(lhs.var()->type());
      auto zero = MachineInt::zero(type->bit_width(), type->sign());
      this->_inv.normal().int_assign(lhs.var(), zero);
    } else if (rhs.is_pointer_var()) {
      this->_inv.normal()
          .scalar_pointer_to_int(lhs.var(),
                                 rhs.var(),
                                 this->_mem_factory.get_absolute_zero());
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute a conversion from integer to pointer
  void exec_int_to_ptr_conv(const ScalarLit& lhs, const ScalarLit& rhs) {
    if (rhs.is_machine_int()) {
      MachineInt addr = rhs.machine_int();

      if (addr.is_zero()) {
        this->_inv.normal().pointer_assign_null(lhs.var());
      } else {
        addr = addr.cast(this->_data_layout.pointers.bit_width, Unsigned);
        this->_inv.normal()
            .pointer_assign(lhs.var(),
                            this->_mem_factory.get_absolute_zero(),
                            Nullity::non_null());
        this->_inv.normal().pointer_assign(lhs.var(), lhs.var(), addr);
      }
    } else if (rhs.is_machine_int_var()) {
      this->_inv.normal()
          .scalar_int_to_pointer(lhs.var(),
                                 rhs.var(),
                                 this->_mem_factory.get_absolute_zero());
    } else {
      ikos_unreachable("unexpected operand");
    }
  }

  /// \brief Execute a bitcast
  ///
  /// Valid bitcasts are:
  ///   * pointer casts: A* to B*
  ///   * primitive type casts with the same bit-width
  ///
  /// A primitive type is either an integer, a floating point or a vector
  /// of integers or floating points.
  void exec_bitcast(ar::UnaryOperation* s,
                    const Literal& lhs,
                    const Literal& rhs) {
    if (rhs.is_var()) {
      this->_inv.normal().uninit_assert_initialized(rhs.var());
    }

    if (lhs.is_scalar()) {
      this->exec_bitcast(s, lhs.scalar(), rhs);
    } else if (lhs.is_aggregate()) {
      this->exec_bitcast(s, lhs.aggregate(), rhs);
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute a bitcast with a scalar left hand side
  void exec_bitcast(ar::UnaryOperation* s,
                    const ScalarLit& lhs,
                    const Literal& rhs) {
    if (lhs.is_pointer_var()) {
      // Pointer cast: A* to B*
      this->assign(lhs, rhs.scalar());
    } else if (lhs.is_machine_int_var()) {
      if (rhs.is_scalar() && rhs.scalar().is_machine_int()) {
        // Sign cast: (u|s)iN to (u|s)iN
        auto type = ar::cast< ar::IntegerType >(s->result()->type());
        this->_inv.normal()
            .int_assign(lhs.var(),
                        rhs.scalar().machine_int().cast(type->bit_width(),
                                                        type->sign()));
      } else if (rhs.is_scalar() && rhs.scalar().is_machine_int_var()) {
        // Sign cast: (u|s)iN to (u|s)iN
        this->_inv.normal().int_apply(IntUnaryOperator::SignCast,
                                      lhs.var(),
                                      rhs.scalar().var());
      } else {
        this->_inv.normal().int_assign_nondet(lhs.var());
      }
    } else {
      ikos_unreachable("unexpected left hand side");
    }
  }

  /// \brief Execute a bitcast with an aggregate left hand side
  void exec_bitcast(ar::UnaryOperation* /*s*/,
                    const AggregateLit& lhs,
                    const Literal& rhs) {
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    if (rhs.is_scalar()) {
      Variable* ptr = this->init_aggregate_memory(lhs);
      this->_inv.normal().mem_forget_reachable(ptr);
    } else if (rhs.is_aggregate()) {
      this->assign(lhs, rhs.aggregate());
    } else {
      ikos_unreachable("unreachable");
    }
  }

public:
  /// \brief Execute a BinaryOperation statement
  void exec(ar::BinaryOperation* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    if (s->result()->type()->is_vector()) {
      this->exec_vector_bin_operation(s);
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& left = this->_lit_factory.get_scalar(s->left());
    const ScalarLit& right = this->_lit_factory.get_scalar(s->right());

    switch (s->op()) {
      case ar::BinaryOperation::UAdd:
      case ar::BinaryOperation::SAdd: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::AddNoWrap
                                         : IntBinaryOperator::Add,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::USub:
      case ar::BinaryOperation::SSub: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::SubNoWrap
                                         : IntBinaryOperator::Sub,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UMul:
      case ar::BinaryOperation::SMul: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::MulNoWrap
                                         : IntBinaryOperator::Mul,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UDiv:
      case ar::BinaryOperation::SDiv: {
        this->exec_int_bin_operation(lhs,
                                     s->is_exact() ? IntBinaryOperator::DivExact
                                                   : IntBinaryOperator::Div,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::URem:
      case ar::BinaryOperation::SRem: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::Rem, left, right);
      } break;
      case ar::BinaryOperation::UShl:
      case ar::BinaryOperation::SShl: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::ShlNoWrap
                                         : IntBinaryOperator::Shl,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::ULShr:
      case ar::BinaryOperation::SLShr: {
        this->exec_int_bin_operation(lhs,
                                     s->is_exact()
                                         ? IntBinaryOperator::LShrExact
                                         : IntBinaryOperator::LShr,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UAShr:
      case ar::BinaryOperation::SAShr: {
        this->exec_int_bin_operation(lhs,
                                     s->is_exact()
                                         ? IntBinaryOperator::AShrExact
                                         : IntBinaryOperator::AShr,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UAnd:
      case ar::BinaryOperation::SAnd: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::And, left, right);
      } break;
      case ar::BinaryOperation::UOr:
      case ar::BinaryOperation::SOr: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::Or, left, right);
      } break;
      case ar::BinaryOperation::UXor:
      case ar::BinaryOperation::SXor: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::Xor, left, right);
      } break;
      case ar::BinaryOperation::FAdd: {
        this->exec_float_bin_operation(lhs, ar::BinaryOperation::FAdd, left, right);
      } break;
      case ar::BinaryOperation::FSub: {
        this->exec_float_bin_operation(lhs, ar::BinaryOperation::FSub, left, right);
      } break;
      case ar::BinaryOperation::FMul: {
        this->exec_float_bin_operation(lhs, ar::BinaryOperation::FMul, left, right);
      } break;
      case ar::BinaryOperation::FDiv: {
        this->exec_float_bin_operation(lhs, ar::BinaryOperation::FDiv, left, right);
      } break;
      case ar::BinaryOperation::FRem: {
        this->exec_float_bin_operation(lhs, ar::BinaryOperation::FRem, left, right);
      } break;
      default: {
        ikos_unreachable("unreachable");
      }
    }
  }

private:
  /// \brief Execute an integer binary operation
  void exec_int_bin_operation(const ScalarLit& lhs,
                              IntBinaryOperator op,
                              const ScalarLit& left,
                              const ScalarLit& right) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (left.is_machine_int()) {
      if (right.is_machine_int()) {
        this->_inv.normal().int_assign(lhs.var(), left.machine_int());
        this->_inv.normal().int_apply(op,
                                      lhs.var(),
                                      lhs.var(),
                                      right.machine_int());
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().int_apply(op,
                                      lhs.var(),
                                      left.machine_int(),
                                      right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else if (left.is_machine_int_var()) {
      if (right.is_machine_int()) {
        this->_inv.normal().int_apply(op,
                                      lhs.var(),
                                      left.var(),
                                      right.machine_int());
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().int_apply(op, lhs.var(), left.var(), right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else {
      ikos_unreachable("unexpected left operand");
    }
  }

  /// \brief Resolve a floating point operand to a known constant value
  ///
  /// Returns nullptr when the operand is not a known constant, or is a width we
  /// cannot model. Callers must top out in that case rather than guess.
  const FloatingPoint* fp_value(const ScalarLit& lit) {
    if (lit.is_floating_point()) {
      const FloatingPoint& f = lit.floating_point();
      return f.is_modeled() ? &f : nullptr;
    }
    if (lit.is_floating_point_var()) {
      const FloatingPoint* f = this->_inv.normal().float_get_cst(lit.var());
      return (f != nullptr && f->is_modeled()) ? f : nullptr;
    }
    return nullptr;
  }

  /// \brief Resolve a floating point operand to a known interval
  ///
  /// Returns nullopt when nothing is known, which includes both "variable with no
  /// recorded interval" and "width we cannot model". Callers top out in that case.
  std::optional< core::floating_point::Interval > fp_interval(
      const ScalarLit& lit) {
    using core::floating_point::Interval;

    if (lit.is_floating_point()) {
      const FloatingPoint& f = lit.floating_point();
      if (!f.is_modeled()) {
        return std::nullopt;
      }
      return Interval::point(f);
    }
    if (lit.is_floating_point_var()) {
      const Interval* iv = this->_inv.normal().float_get_interval(lit.var());
      // Nothing recorded is still a usable answer: the whole line, NaN included.
      // Returning top rather than nullopt is what lets a guarded comparison such
      // as `x >= 1.0 && x <= 2.0` narrow a previously unknown variable.
      return iv != nullptr ? *iv : Interval::top_value();
    }
    return std::nullopt;
  }

  /// \brief Compare r*r against d exactly, with no rounding error.
  ///
  /// The naive `r * r <= d` is wrong about a quarter of the time: r*r rounds
  /// back to exactly d even when the true product sits above it, so the test
  /// cannot tell which side of the root r landed on. Measured against exact
  /// integer arithmetic over 200,000 random doubles, the naive form disagreed
  /// with the truth 52,736 times. Using it would make the sqrt bounds unsound.
  ///
  /// The fix extracts the product's rounding error with the standard two-FMA
  /// identity: with p = RN(r*r), fma(r, r, -p) is exactly the residual e, so
  /// r*r == p + e exactly. Comparing that against d is then the same as
  /// comparing e against d - p, and d - p is exact by Sterbenz's lemma because
  /// r = RN(sqrt(d)) keeps p within a factor of two of d. Both sides are
  /// exact, so the comparison is.
  ///
  /// Returns -1 when r*r < d, 0 when equal, +1 when greater.
  template <typename T>
  static int cmp_square_exact(T r, T d) {
    T p = r * r;
    T e = std::fma(r, r, -p);
    T diff = d - p;
    if (e < diff) {
      return -1;
    }
    if (e > diff) {
      return 1;
    }
    return 0;
  }

  /// \brief Outward-rounded image of [lo, hi] under sqrt, for 0 <= lo <= hi.
  ///
  /// sqrt is monotonically non-decreasing on [0, inf), so the image is
  /// [sqrt(lo), sqrt(hi)]; those roots are usually not representable, so the
  /// bounds must go OUTWARD or they are unsound -- the same requirement that
  /// fptrunc hit in run #55. cmp_square_exact says which side of the root the
  /// correctly-rounded result landed on, and one nextafter step corrects it.
  /// Perfect squares need no correction because the comparison comes back
  /// equal, which is what keeps the common cases tight.
  template <typename T>
  static void sqrt_image_outward(T lo, T hi, T& out_lo, T& out_hi) {
    T pinf = std::numeric_limits<T>::infinity();

    // sqrt(+inf) is +inf, and the exact-comparison helper would evaluate
    // inf - inf inside its residual, so short-circuit an infinite upper bound.
    if (std::isinf(hi)) {
      out_hi = pinf;
    } else {
      T rh = std::sqrt(hi);
      out_hi = (cmp_square_exact(rh, hi) < 0) ? std::nextafter(rh, pinf) : rh;
    }

    if (lo <= T(0)) {
      // sqrt(+0.0) is +0.0 and sqrt(-0.0) is -0.0; either bounds the same way.
      out_lo = lo;
    } else {
      T rl = std::sqrt(lo);
      out_lo = (cmp_square_exact(rl, lo) > 0) ? std::nextafter(rl, -pinf) : rl;
    }
  }

  /// \brief Execute llvm.sqrt.
  ///
  /// NaN handling is the part that is easy to get wrong: sqrt of a negative
  /// input is NaN, so an interval reaching below zero makes the result
  /// possibly NaN even though the ordered part is well defined.
  void exec_float_sqrt(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 1);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));

    if (const FloatingPoint* v = this->fp_value(la)) {
      if (!v->is_modeled() ||
          (v->bit_width() != 32 && v->bit_width() != 64)) {
        this->_inv.normal().float_assign_nondet(ret.var());
        return;
      }
      if (v->bit_width() == 32) {
        this->_inv.normal().float_assign_cst(
            ret.var(), FloatingPoint::from_float(std::sqrt(v->to_float())));
      } else {
        this->_inv.normal().float_assign_cst(
            ret.var(), FloatingPoint::from_double(std::sqrt(v->to_double())));
      }
      return;
    }

    auto iv = this->fp_interval(la);
    if (!iv.has_value() || iv->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!iv->lo.is_modeled() || !iv->hi.is_modeled() ||
        (iv->lo.bit_width() != 32 && iv->lo.bit_width() != 64)) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    // Sign tests via to_double are exact: widening float to double loses
    // nothing, so this is safe at either width.
    const double lo_d = iv->lo.to_double();
    const double hi_d = iv->hi.to_double();
    bool nan_out = iv->may_nan;

    FloatingPoint lo = iv->lo;
    if (hi_d < 0.0) {
      // Every input is negative, so every result is NaN. Say exactly that: the
      // definitely-NaN representation is an empty ordered range with may_nan
      // set, which is what `Interval::point(nan)` produces, and
      // `float_assign_interval` carries it through.
      //
      // Topping out here instead is why `sqrt(1.0 - x*x)` stayed unknown at
      // x = 1.01 while the same expression at x = 2.0 -- a folded literal, so
      // it took the constant path above -- was known-NaN. The two differ only
      // in how the negative argument was built, and only one of them said so.
      this->_inv.normal().float_assign_interval(
          ret.var(),
          core::floating_point::Interval{
              core::floating_point::Interval::pos_inf(),
              core::floating_point::Interval::neg_inf(), true});
      return;
    }
    if (lo_d < 0.0) {
      // The interval straddles zero: the negative part yields NaN, so carry
      // that possibility and bound the ordered part from +0.0 up.
      nan_out = true;
      lo = FloatingPoint::from_bits(iv->lo.bit_width(), 0);
    }

    core::floating_point::Interval out{iv->lo, iv->hi, nan_out};

    if (lo.bit_width() == 32) {
      float l32 = 0.0f;
      float h32 = 0.0f;
      sqrt_image_outward<float>(lo.to_float(), iv->hi.to_float(), l32, h32);
      out = core::floating_point::Interval{FloatingPoint::from_float(l32),
                                         FloatingPoint::from_float(h32),
                                         nan_out};
    } else {
      double l64 = 0.0;
      double h64 = 0.0;
      sqrt_image_outward<double>(lo.to_double(), iv->hi.to_double(), l64, h64);
      out = core::floating_point::Interval{FloatingPoint::from_double(l64),
                                         FloatingPoint::from_double(h64),
                                         nan_out};
    }

    if (out.is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    this->_inv.normal().float_assign_interval(ret.var(), out);
  }

  /// \brief Base selector shared by llvm.log, llvm.log2 and llvm.log10.
  enum class LogBase { Nat, Two, Ten };

  /// \brief Machine value of log_base(v) at the operand's own width.
  ///
  /// This is the value the program itself computes, which is what the domain
  /// tracks: the branch models IEEE machine values, not ideal reals, the same
  /// way `0.1 + 0.2` is held as 0x1.3333333333334p-2 rather than 0.3.
  static FloatingPoint log_point(const FloatingPoint& v, LogBase base) {
    if (v.bit_width() == 32) {
      float x = v.to_float();
      float r = (base == LogBase::Nat)
                    ? std::log(x)
                    : ((base == LogBase::Two) ? std::log2(x) : std::log10(x));
      return FloatingPoint::from_float(r);
    }
    double x = v.to_double();
    double r = (base == LogBase::Nat)
                   ? std::log(x)
                   : ((base == LogBase::Two) ? std::log2(x) : std::log10(x));
    return FloatingPoint::from_double(r);
  }

  /// \brief Is `v` an exact power of two?
  ///
  /// Bit test, not a round trip: log2 of a positive normal power of two is an
  /// exact integer, so that bound needs no widening. Mantissa zero, exponent
  /// neither zero (subnormal/zero) nor all-ones (inf/NaN), sign positive.
  static bool is_exact_pow2(double v) {
    if (!(v > 0.0) || !std::isfinite(v)) {
      return false;
    }
    return (FloatingPoint::from_double(v).bits() & 0x000FFFFFFFFFFFFFULL) == 0;
  }

  /// \brief Is `v` an exact power of ten?
  ///
  /// 10^k = 2^k * 5^k is representable only while 5^k fits the 53-bit
  /// significand, which caps k at 22; negative k would need 1/5^k to be a
  /// dyadic rational, which it never is. So the whole candidate set is
  /// 10^0..10^22 and each step is exact.
  static bool is_exact_pow10(double v) {
    if (!(v >= 1.0) || !std::isfinite(v)) {
      return false;
    }
    double p = 1.0;
    for (int k = 0; k <= 22; ++k) {
      if (FloatingPoint::from_double(p).bits() ==
          FloatingPoint::from_double(v).bits()) {
        return true;
      }
      p *= 10.0;
    }
    return false;
  }

  /// \brief Is log_base(v) exactly representable?
  ///
  /// Drives whether a bound has to be widened. For base e the only exact
  /// finite case is v == 1 giving 0: by Lindemann-Weierstrass e^a is
  /// transcendental for nonzero algebraic a, so log of any other algebraic
  /// value is irrational and cannot land on a machine number.
  static bool log_is_exact(double v, LogBase base) {
    // log_b(1) == 0 exactly for every base, and for base e that is the only
    // exact finite value: by Lindemann-Weierstrass e^a is transcendental for
    // nonzero algebraic a, so the log of any other algebraic value is
    // irrational and cannot land on a machine number.
    //
    // Compared on bit patterns rather than with `==` so -Wfloat-equal stays
    // satisfied, matching how the rest of this file handles exact float tests.
    if (FloatingPoint::from_double(v).bits() ==
        FloatingPoint::from_double(1.0).bits()) {
      return true;
    }
    if (base == LogBase::Two) {
      return is_exact_pow2(v);
    }
    if (base == LogBase::Ten) {
      return is_exact_pow10(v);
    }
    return false;
  }

  /// \brief One end of the log image of an interval, widened outward unless
  /// the value is provably exact.
  ///
  /// Widening is what keeps the bound sound against libm being off by an ulp
  /// or the host and target disagreeing, mirroring the outward round in
  /// sqrt_image_outward. Skipping it for exact results is what keeps
  /// `log(1) == 0` and `log2(2) == 1` from degrading into a denormal or an
  /// off-by-one bound -- without that, `x in [1,2] -> log(x) >= 0` fails
  /// because nextafter(0.0, -inf) is a tiny negative denormal.
  static FloatingPoint log_bound(const FloatingPoint& v, LogBase base, bool up) {
    FloatingPoint r = log_point(v, base);
    if (r.is_nan() || r.is_inf()) {
      return r;
    }
    if (log_is_exact(v.to_double(), base)) {
      return r;
    }
    if (v.bit_width() == 32) {
      return FloatingPoint::from_float(std::nextafterf(
          r.to_float(), up ? std::numeric_limits<float>::infinity()
                          : -std::numeric_limits<float>::infinity()));
    }
    return FloatingPoint::from_double(std::nextafter(
        r.to_double(), up ? std::numeric_limits<double>::infinity()
                         : -std::numeric_limits<double>::infinity()));
  }

  /// \brief Execute llvm.log, llvm.log2 or llvm.log10.
  ///
  /// All three are monotonically increasing on (0, inf), so the ordered image
  /// of a positive interval is just [log(lo), log(hi)].
  ///
  /// The NaN boundary is the part that matters. log(x) is NaN for every x < 0,
  /// so an interval lying entirely below zero gives a result that is
  /// DEFINITELY NaN. That is carried as an empty ordered range with may_nan
  /// set -- the shape Interval::point(NaN) produces and is_definitely_nan()
  /// recognises -- which lets the prover discharge isnan() and refute !isnan()
  /// instead of topping out and knowing nothing.
  ///
  /// log(+0) is -inf; the ordered range carries that rather than treating it
  /// as an error.
  void exec_float_log(ar::CallBase* call, LogBase base) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 1);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));

    // Constant operand: evaluate exactly at the operand width, the same way
    // exec_float_sqrt does. This is the path that makes isnan(log(-2.0))
    // provable rather than merely unprovable -- float_assign_cst routes a NaN
    // result through Interval::point, which yields definitely-NaN.
    if (const FloatingPoint* v = this->fp_value(la)) {
      if (!v->is_modeled() ||
          (v->bit_width() != 32 && v->bit_width() != 64)) {
        this->_inv.normal().float_assign_nondet(ret.var());
        return;
      }
      if (v->bit_width() == 32) {
        float x = v->to_float();
        float r = (base == LogBase::Nat)
                      ? std::log(x)
                      : ((base == LogBase::Two) ? std::log2(x) : std::log10(x));
        this->_inv.normal().float_assign_cst(ret.var(),
                                           FloatingPoint::from_float(r));
      } else {
        double x = v->to_double();
        double r = (base == LogBase::Nat)
                       ? std::log(x)
                       : ((base == LogBase::Two) ? std::log2(x) : std::log10(x));
        this->_inv.normal().float_assign_cst(ret.var(),
                                           FloatingPoint::from_double(r));
      }
      return;
    }

    auto iv = this->fp_interval(la);
    if (!iv.has_value() || iv->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!iv->lo.is_modeled() || !iv->hi.is_modeled() ||
        (iv->lo.bit_width() != 32 && iv->lo.bit_width() != 64)) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    const uint16_t bw = iv->lo.bit_width();
    const double lo_d = iv->lo.to_double();
    const double hi_d = iv->hi.to_double();

    if (hi_d < 0.0) {
      // Every input is negative, so every result is NaN: empty ordered range,
      // may_nan set. Width-matched infinities keep lo > hi so that
      // ordered_empty() holds, which is what makes this definitely-NaN rather
      // than merely possibly-NaN.
      FloatingPoint pinf = (bw == 32)
                              ? FloatingPoint::from_float(
                                    std::numeric_limits<float>::infinity())
                              : FloatingPoint::from_double(
                                    std::numeric_limits<double>::infinity());
      FloatingPoint ninf = (bw == 32)
                              ? FloatingPoint::from_float(
                                    -std::numeric_limits<float>::infinity())
                              : FloatingPoint::from_double(
                                    -std::numeric_limits<double>::infinity());
      this->_inv.normal().float_assign_interval(
          ret.var(), core::floating_point::Interval{pinf, ninf, true});
      return;
    }

    // Straddling zero: the negative part yields NaN, and the ordered part is
    // the image of the non-negative portion, so clamp the low end at +0.0.
    bool nan_out = iv->may_nan || (lo_d < 0.0);
    FloatingPoint lo =
        (lo_d < 0.0) ? FloatingPoint::from_bits(bw, 0) : iv->lo;

    FloatingPoint out_lo = log_bound(lo, base, /* up = */ false);
    FloatingPoint out_hi = log_bound(iv->hi, base, /* up = */ true);

    core::floating_point::Interval out{out_lo, out_hi, nan_out};
    if (out.is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    this->_inv.normal().float_assign_interval(ret.var(), out);
  }

  /// \brief Execute llvm.pow.
  ///
  /// Two things make this harder than the monotone functions above.
  ///
  /// NaN depends on BOTH operands: pow(x, y) is NaN when x < 0 and y is not
  /// an integer. Corners alone cannot see this -- with x in [-1, 1] and y in
  /// [2, 3] every corner is ordered, yet x = -1 with y = 2.5 is NaN. So on
  /// top of the corner scan, a base that can go negative forces may_nan unless
  /// the exponent is a point holding an integer.
  ///
  /// Monotonicity depends on the exponent's sign: x**y increases in x when
  /// y > 0 and decreases when y < 0. The ordered hull is therefore taken over
  /// all four corners rather than assumed to be [pow(lo,lo), pow(hi,hi)].
  ///
  /// All-corners-NaN is deliberately NOT read as definitely-NaN: as the
  /// [2, 3] example shows, ordered interior values can survive it. That case
  /// tops out instead.
  void exec_float_pow(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 2);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& lb = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& le = this->_lit_factory.get_scalar(call->argument(1));

    // Both operands constant: exact, and NaN shows up as a NaN constant.
    if (const FloatingPoint* xb = this->fp_value(lb)) {
      const FloatingPoint* ye = this->fp_value(le);
      if (xb->is_modeled() && ye != nullptr && ye->is_modeled() &&
          xb->bit_width() == ye->bit_width() &&
          (xb->bit_width() == 32 || xb->bit_width() == 64)) {
        if (xb->bit_width() == 32) {
          this->_inv.normal().float_assign_cst(
              ret.var(),
              FloatingPoint::from_float(std::pow(xb->to_float(),
                                               ye->to_float())));
        } else {
          this->_inv.normal().float_assign_cst(
              ret.var(),
              FloatingPoint::from_double(std::pow(xb->to_double(),
                                                ye->to_double())));
        }
        return;
      }
    }

    auto ia = this->fp_interval(lb);
    auto ib = this->fp_interval(le);
    if (!ia.has_value() || !ib.has_value() || ia->is_empty() || ib->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!ia->lo.is_modeled() || !ia->hi.is_modeled() ||
        !ib->lo.is_modeled() || !ib->hi.is_modeled() ||
        (ia->lo.bit_width() != 32 && ia->lo.bit_width() != 64)) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    const double xlos[] = {ia->lo.to_double(), ia->hi.to_double()};
    const double ylos[] = {ib->lo.to_double(), ib->hi.to_double()};

    bool any_ordered = false;
    bool any_nan_corner = false;
    double hlo = 0.0;
    double hhi = 0.0;
    for (double x : xlos) {
      for (double y : ylos) {
        double r = std::pow(x, y);
        if (std::isnan(r)) {
          any_nan_corner = true;
          continue;
        }
        if (!any_ordered) {
          hlo = r;
          hhi = r;
          any_ordered = true;
        } else {
          hlo = std::min(hlo, r);
          hhi = std::max(hhi, r);
        }
      }
    }

    // A negative base with a non-integral exponent is NaN even when no corner
    // shows it, so widen may_nan here rather than trusting the corner scan.
    // Integrality is tested on bit patterns -- floor() of an integral double
    // is that same double, so equal bits means no fractional part -- which
    // keeps -Wfloat-equal satisfied the way the rest of this file does.
    const double e_d = ib->lo.to_double();
    bool exp_is_integral_point =
        (ib->lo.bits() == ib->hi.bits()) && std::isfinite(e_d) &&
        (FloatingPoint::from_double(std::floor(e_d)).bits() ==
         FloatingPoint::from_double(e_d).bits());
    bool nan_out = ia->may_nan || ib->may_nan || any_nan_corner ||
                  ((ia->lo.to_double() < 0.0) && !exp_is_integral_point);

    if (!any_ordered) {
      // Nothing to bound the ordered part with. Top out -- but do NOT read
      // all-NaN corners as definitely-NaN; interior ordered values can exist.
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    // Widen the corner hull by one ulp outward to absorb the rounding of each
    // corner evaluation and any interior value the corners did not attain.
    //
    // Unlike log there is no cheap exactness test here, so `x in [2,3] ->
    // pow(x,2) >= 4` is not provable: pow(2,2) is exactly 4.0 but the hull
    // is pushed to 3.9999999999999996. That is sound, just not tight. The
    // log path avoids it because powers of the base are decidable from bits;
    // exactness of a general a**b is not, without carrying an error term the
    // way Fluctuat does.
    const uint16_t bw = ia->lo.bit_width();
    if (bw == 32) {
      float l = std::nextafterf(static_cast<float>(hlo),
                               -std::numeric_limits<float>::infinity());
      float h = std::nextafterf(static_cast<float>(hhi),
                               std::numeric_limits<float>::infinity());
      this->_inv.normal().float_assign_interval(
          ret.var(),
          core::floating_point::Interval{
              FloatingPoint::from_float(l), FloatingPoint::from_float(h),
              nan_out});
    } else {
      double l =
          std::nextafter(hlo, -std::numeric_limits<double>::infinity());
      double h = std::nextafter(hhi, std::numeric_limits<double>::infinity());
      this->_inv.normal().float_assign_interval(
          ret.var(),
          core::floating_point::Interval{
              FloatingPoint::from_double(l), FloatingPoint::from_double(h),
              nan_out});
    }
  }

  /// \brief Execute llvm.copysign.
  ///
  /// Magnitude comes from `a`, the sign bit comes from `b`. The magnitude image
  /// is the same |a| hull used for fabs in run #59 -- magnitude is monotonic
  /// in |x| -- with `b`'s sign then applied to it.
  ///
  /// The sign is only determined when `b`'s sign bit is known. A `b` interval
  /// that reaches zero leaves the sign open, because -0.0 and +0.0 sit at the
  /// same numeric bound but carry different sign bits. A possibly-NaN `b` also
  /// leaves it open: copysign reads the sign BIT of a NaN, so
  /// copysign(2.5, -NaN) is -2.5 rather than NaN.
  ///
  /// `b` can never introduce a NaN, so may_nan comes from `a` alone. With the
  /// sign unknown the result is the symmetric hull, which still carries the
  /// magnitude bound through -- better than topping out.
  void exec_float_copysign(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 2);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& lb = this->_lit_factory.get_scalar(call->argument(1));

    const FloatingPoint* va = this->fp_value(la);
    const FloatingPoint* vb = this->fp_value(lb);

    if (va != nullptr && vb != nullptr) {
      if (!va->is_modeled() || !vb->is_modeled() ||
          va->bit_width() != vb->bit_width() ||
          (va->bit_width() != 32 && va->bit_width() != 64)) {
        this->_inv.normal().float_assign_nondet(ret.var());
        return;
      }
      this->_inv.normal().float_assign_cst(ret.var(),
                                         FloatingPoint::copysign(*va, *vb));
      return;
    }

    auto iva = this->fp_interval(la);
    if (!iva.has_value() || iva->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!iva->lo.is_modeled() || !iva->hi.is_modeled() ||
        (iva->lo.bit_width() != 32 && iva->lo.bit_width() != 64)) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    // |a| hull. Bounds are negated as FloatingPoint rather than round-tripping
    // through double, so a narrow value is never relabelled as a wider one
    // (run #28's width lesson).
    const double alo = iva->lo.to_double();
    const double ahi = iva->hi.to_double();
    FloatingPoint mlo = iva->lo;
    FloatingPoint mhi = iva->hi;
    if (alo >= 0.0) {
      mlo = iva->lo;
      mhi = iva->hi;
    } else if (ahi <= 0.0) {
      mlo = FloatingPoint::neg(iva->hi);
      mhi = FloatingPoint::neg(iva->lo);
    } else {
      FloatingPoint nlo = FloatingPoint::neg(iva->lo);
      mlo = FloatingPoint::from_bits(iva->lo.bit_width(), 0); // +0.0
      mhi = (nlo.to_double() >= ahi) ? nlo : iva->hi;
    }

    // Which sign, if any, does `b` pin down?
    int sgn = 0;
    if (vb != nullptr && vb->is_modeled() &&
        vb->bit_width() == iva->lo.bit_width()) {
      sgn = std::signbit(vb->to_double()) ? -1 : 1;
    } else {
      auto ivb = this->fp_interval(lb);
      if (ivb.has_value() && !ivb->is_empty() && !ivb->may_nan &&
          ivb->lo.is_modeled() && ivb->hi.is_modeled()) {
        const double blo = ivb->lo.to_double();
        const double bhi = ivb->hi.to_double();
        if (blo > 0.0) {
          sgn = 1;
        } else if (bhi < 0.0) {
          sgn = -1;
        }
      }
    }

    FloatingPoint out_lo = mhi;
    FloatingPoint out_hi = mhi;
    if (sgn > 0) {
      out_lo = mlo;
      out_hi = mhi;
    } else if (sgn < 0) {
      out_lo = FloatingPoint::neg(mhi);
      out_hi = FloatingPoint::neg(mlo);
    } else {
      out_lo = FloatingPoint::neg(mhi);
      out_hi = mhi;
    }

    core::floating_point::Interval out{out_lo, out_hi, iva->may_nan};
    if (out.is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    this->_inv.normal().float_assign_interval(ret.var(), out);
  }

  /// \brief How to round a float to an integer-valued float.
  enum class RoundMode { Floor, Ceil, Trunc, HalfAway, HalfEven };

  /// \brief Execute llvm.floor / llvm.ceil / llvm.trunc / llvm.round / llvm.rint.
  ///
  /// All five modes are monotonically non-decreasing and return exact
  /// integers, so the image of [lo, hi] is exactly [f(lo), f(hi)]: the
  /// endpoints are attained, and being integers they are representable, so no
  /// outward rounding is required. That is the opposite of fptrunc (run #55),
  /// which rounds and therefore must widen to stay sound. Past 2^p every float
  /// already is an integer, so f(x) == x there and exactness holds trivially.
  ///
  /// The modes differ only in the tie rule, which the native functions get
  /// right: floor/ceil/trunc have no ties to speak of, round breaks away from
  /// zero (round(2.5) is 3, round(-2.5) is -3), and rint breaks to even
  /// (rint(2.5) and rint(3.5) are both 2).
  ///
  /// NaN propagates -- floor(NaN) is NaN -- so may_nan carries through.
  void exec_float_round(ar::CallBase* call, RoundMode mode) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 1);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));

    // Applied at the operand's own width. Using the native functions keeps the
    // signed-zero behaviour exact (trunc(-0.5) is -0.0, not +0.0).
    auto apply_d = [mode](double v) -> double {
      double r = v;
      switch (mode) {
      case RoundMode::Floor:
        r = std::floor(v);
        break;
      case RoundMode::Ceil:
        r = std::ceil(v);
        break;
      case RoundMode::Trunc:
        r = std::trunc(v);
        break;
      case RoundMode::HalfAway:
        r = std::round(v);
        break;
      case RoundMode::HalfEven:
        // std::rint follows the current rounding direction, which is exactly
        // what llvm.rint means, so this is the faithful match. IKOS assumes
        // the default round-to-nearest-even mode throughout, same as its
        // existing single-threaded caveat.
        r = std::rint(v);
        break;
      }
      return r;
    };
    auto apply_f = [mode](float v) -> float {
      float r = v;
      switch (mode) {
      case RoundMode::Floor:
        r = std::floor(v);
        break;
      case RoundMode::Ceil:
        r = std::ceil(v);
        break;
      case RoundMode::Trunc:
        r = std::trunc(v);
        break;
      case RoundMode::HalfAway:
        r = std::round(v);
        break;
      case RoundMode::HalfEven:
        r = std::rint(v);
        break;
      }
      return r;
    };

    if (const FloatingPoint* v = this->fp_value(la)) {
      if (!v->is_modeled() ||
          (v->bit_width() != 32 && v->bit_width() != 64)) {
        this->_inv.normal().float_assign_nondet(ret.var());
        return;
      }
      if (v->bit_width() == 32) {
        this->_inv.normal().float_assign_cst(
            ret.var(), FloatingPoint::from_float(apply_f(v->to_float())));
      } else {
        this->_inv.normal().float_assign_cst(
            ret.var(), FloatingPoint::from_double(apply_d(v->to_double())));
      }
      return;
    }

    auto iv = this->fp_interval(la);
    if (!iv.has_value() || iv->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!iv->lo.is_modeled() || !iv->hi.is_modeled() ||
        (iv->lo.bit_width() != 32 && iv->lo.bit_width() != 64)) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    const bool is32 = (iv->lo.bit_width() == 32);
    FloatingPoint lo = is32
        ? FloatingPoint::from_float(apply_f(iv->lo.to_float()))
        : FloatingPoint::from_double(apply_d(iv->lo.to_double()));
    FloatingPoint hi = is32
        ? FloatingPoint::from_float(apply_f(iv->hi.to_float()))
        : FloatingPoint::from_double(apply_d(iv->hi.to_double()));

    core::floating_point::Interval out{lo, hi, iv->may_nan};
    if (out.is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    this->_inv.normal().float_assign_interval(ret.var(), out);
  }

  /// \brief Execute llvm.maxnum or llvm.minnum (IEEE-754 maxNum / minNum).
  ///
  /// The subtlety is that these do NOT propagate NaN: if exactly one operand is
  /// NaN, the other, non-NaN operand is returned. So a NaN-capable operand does
  /// not poison the result the way it does for `+` or `*`; instead it drags the
  /// bound toward its own far edge.
  ///
  /// For maxnum over A = [alo, ahi] (NaN optional) and B = [blo, bhi]:
  ///
  ///   neither NaN -> {max(x,y)}               = [max(alo,blo), max(ahi,bhi)]
  ///   only A      -> ... union [blo, bhi]     => lower bound becomes blo
  ///   only B      -> ... union [alo, ahi]     => lower bound becomes alo
  ///   both        -> ... union both, plus NaN  => lower bound min(alo,blo)
  ///
  /// The upper bound stays max(ahi, bhi) in every case: absorbing a NaN yields
  /// one of the operands, and both are already <= max(ahi, bhi). Only the lower
  /// bound loosens, and only by as much as NaN-absorption requires. minnum is
  /// the mirror image.
  void exec_float_maxmin(ar::CallBase* call, bool is_max) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 2);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& lb = this->_lit_factory.get_scalar(call->argument(1));

    // Both operands known: use the native operation at the operands' width. That
    // matches the hardware exactly, including the signed-zero tie rule --
    // maxnum(+0,-0) is +0.0 while minnum(+0,-0) is -0.0 -- which is fiddly to
    // reimplement and easy to get backwards.
    const FloatingPoint* va = this->fp_value(la);
    const FloatingPoint* vb = this->fp_value(lb);
    if (va != nullptr && vb != nullptr) {
      if (!va->is_modeled() || !vb->is_modeled() ||
          va->bit_width() != vb->bit_width() ||
          (va->bit_width() != 32 && va->bit_width() != 64)) {
        this->_inv.normal().float_assign_nondet(ret.var());
        return;
      }
      if (va->bit_width() == 32) {
        float a = va->to_float();
        float b = vb->to_float();
        this->_inv.normal().float_assign_cst(
            ret.var(), FloatingPoint::from_float(is_max ? std::fmax(a, b)
                                                      : std::fmin(a, b)));
      } else {
        double a = va->to_double();
        double b = vb->to_double();
        this->_inv.normal().float_assign_cst(
            ret.var(), FloatingPoint::from_double(is_max ? std::fmax(a, b)
                                                       : std::fmin(a, b)));
      }
      return;
    }

    auto ia = this->fp_interval(la);
    auto ib = this->fp_interval(lb);
    if (!ia.has_value() || !ib.has_value() || ia->is_empty() || ib->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!ia->lo.is_modeled() || !ia->hi.is_modeled() ||
        !ib->lo.is_modeled() || !ib->hi.is_modeled()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    double alo = ia->lo.to_double();
    double ahi = ia->hi.to_double();
    double blo = ib->lo.to_double();
    double bhi = ib->hi.to_double();
    bool na = ia->may_nan;
    bool nb = ib->may_nan;

    // Only both-NaN can produce a NaN result: absorbing requires exactly one.
    // Bounds are selected as FloatingPoints directly rather than by matching a
    // computed double back, which keeps -Wfloat-equal satisfied and avoids
    // relying on a default constructor.
    bool out_nan = na && nb;
    FloatingPoint lo_fp = ia->lo;
    FloatingPoint hi_fp = ia->hi;

    if (is_max) {
      hi_fp = (ahi > bhi) ? ia->hi : ib->hi;
      if (na && nb) {
        lo_fp = (alo < blo) ? ia->lo : ib->lo;
      } else if (na) {
        lo_fp = ib->lo;
      } else if (nb) {
        lo_fp = ia->lo;
      } else {
        lo_fp = (alo > blo) ? ia->lo : ib->lo;
      }
    } else {
      lo_fp = (alo < blo) ? ia->lo : ib->lo;
      if (na && nb) {
        hi_fp = (ahi > bhi) ? ia->hi : ib->hi;
      } else if (na) {
        hi_fp = ib->hi;
      } else if (nb) {
        hi_fp = ia->hi;
      } else {
        hi_fp = (ahi < bhi) ? ia->hi : ib->hi;
      }
    }

    core::floating_point::Interval out{lo_fp, hi_fp, out_nan};
    if (out.is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    this->_inv.normal().float_assign_interval(ret.var(), out);
  }

  /// \brief Execute LLVM's frem (C's fmod): truncated remainder.
  ///
  /// Semantics that matter here:
  ///   - the result takes the sign of the DIVIDEND, not the divisor;
  ///   - |fmod(x, y)| < |y| whenever x is finite and y is finite and non-zero;
  ///   - fmod(NaN, y), fmod(x, NaN), fmod(inf, y) and fmod(x, 0) are NaN.
  ///
  /// The constant case is computed with the host's own fmod/fmodf, so it is
  /// ground truth by construction rather than a hand-rolled sign rule -- that
  /// hand-rolled rules are easy to get wrong is exactly why run #34 declined this.
  void exec_float_frem(const ScalarLit& lhs,
                       std::optional< core::floating_point::Interval> const& li,
                       std::optional< core::floating_point::Interval> const& ri) {
    using core::floating_point::Interval;

    if (!li.has_value() || !ri.has_value() || li->is_empty() || ri->is_empty()) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }
    if (!li->lo.is_modeled() || !li->hi.is_modeled() ||
        !ri->lo.is_modeled() || !ri->hi.is_modeled()) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }

    uint16_t w = li->lo.bit_width();

    // --- both operands known: evaluate with the host's fmod at that width ---
    if (li->is_point() && ri->is_point()) {
      double a = li->lo.to_double();
      double b = ri->lo.to_double();
      double r = (w == 32)
                     ? static_cast<double>(fmodf(static_cast<float>(a),
                                               static_cast<float>(b)))
                     : fmod(a, b);
      FloatingPoint out = (w == 32)
                             ? FloatingPoint::from_float(static_cast<float>(r))
                             : FloatingPoint::from_double(r);
      core::floating_point::Interval res = Interval::point(out);
      res.may_nan = std::isnan(r);
      this->_inv.normal().float_assign_interval(lhs.var(), res);
      return;
    }

    // --- interval case ---
    // |fmod(x, y)| < |y| needs y finite and non-zero. If y can be zero or is
    // unbounded there is no usable bound, so top out rather than guess.
    double ylo = ri->lo.to_double();
    double yhi = ri->hi.to_double();
    double pinf = Interval::pos_inf().to_double();
    bool y_zero_reachable = ylo <= 0.0 && yhi >= 0.0;
    bool y_finite = ylo > -pinf && yhi < pinf;
    if (y_zero_reachable || !y_finite || ri->may_nan) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }

    // Largest divisor magnitude bounds the remainder magnitude.
    FloatingPoint ymax = (std::fabs(ylo) > std::fabs(yhi))
                            ? FloatingPoint::abs(ri->lo)
                            : FloatingPoint::abs(ri->hi);

    // |fmod(x, y)| < |y| is STRICT, but the interval here is closed. The result
    // is itself a float, so the tightest closed bound that still excludes |y| is
    // the predecessor float: any float strictly below |y| is <= nextafter(|y|,0).
    // Without this step `fmod(x, 3.0) < 3.0` is unprovable, which is the single
    // most natural thing to assert about a wrapped remainder.
    double m = ymax.to_double();
    m = (w == 32)
            ? static_cast<double>(std::nextafterf(static_cast<float>(m), 0.0f))
            : std::nextafter(m, 0.0);
    ymax = (w == 32) ? FloatingPoint::from_float(static_cast<float>(m))
                     : FloatingPoint::from_double(m);

    double xlo = li->lo.to_double();
    double xhi = li->hi.to_double();

    // fmod(x, y) is NaN when x is infinite, so an x bound that can reach
    // infinity makes the result possibly NaN.
    bool x_inf_reachable = xlo <= -pinf || xhi >= pinf;

    Interval out = *li;
    out.may_nan = li->may_nan || x_inf_reachable;

    if (xlo >= 0.0) {
      // Non-negative dividend -> remainder in [0, |y|).
      out.lo = FloatingPoint::from_bits(w, 0);  // +0.0
      out.hi = ymax;
    } else if (xhi <= 0.0) {
      // Non-positive dividend -> remainder in (-|y|, 0].
      out.lo = FloatingPoint::neg(ymax);
      out.hi = FloatingPoint::from_bits(w, 0);  // +0.0
    } else {
      // Sign of the dividend is open, so both signs of remainder are possible.
      out.lo = FloatingPoint::neg(ymax);
      out.hi = ymax;
    }
    this->_inv.normal().float_assign_interval(lhs.var(), out);
  }

  /// \brief Execute llvm.fabs.
  ///
  /// Magnitude is monotonic in |x|, so the image of an interval is attained at
  /// an endpoint and needs no outward rounding -- the same argument that makes
  /// the run #58 squaring rule exact.
  ///
  /// Every step works on the FloatingPoint bounds themselves rather than
  /// round-tripping through double, so the operand's width is preserved. Run
  /// #28 is the cautionary tale: relabelling a binary32 value as binary64
  /// silently changes what it means.
  void exec_float_abs(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 1);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));

    if (const FloatingPoint* v = this->fp_value(la)) {
      if (!v->is_modeled()) {
        this->_inv.normal().float_assign_nondet(ret.var());
        return;
      }
      this->_inv.normal().float_assign_cst(ret.var(), FloatingPoint::abs(*v));
      return;
    }

    auto iv = this->fp_interval(la);
    if (!iv.has_value() || iv->is_empty()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    if (!iv->lo.is_modeled() || !iv->hi.is_modeled()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    double lo = iv->lo.to_double();
    double hi = iv->hi.to_double();

    // may_nan carries through unchanged: fabs(NaN) is NaN.
    core::floating_point::Interval out = *iv;

    if (lo >= 0.0) {
      // Already non-negative; the image is the interval itself.
    } else if (hi <= 0.0) {
      // Entirely negative: negating flips and swaps the bounds.
      out.lo = FloatingPoint::neg(iv->hi);
      out.hi = FloatingPoint::neg(iv->lo);
    } else {
      // Straddles zero, so zero is reachable and is the minimum. The maximum is
      // whichever endpoint has the larger magnitude.
      out.lo = FloatingPoint::from_bits(iv->lo.bit_width(), 0);  // +0.0
      out.hi = (lo < -hi) ? FloatingPoint::abs(iv->lo) : iv->hi;
    }
    this->_inv.normal().float_assign_interval(ret.var(), out);
  }

  /// \brief Execute a fused multiply-add intrinsic.
  ///
  /// `strict` separates the two LLVM intrinsics, which are NOT equivalent:
  ///
  ///   llvm.fma      -- strict IEEE fused operation. Exactly one rounding, so
  ///                   the result is determined: round(exact(a*b) + c).
  ///
  ///   llvm.fmuladd  -- a contraction HINT. The backend fuses only where that
  ///                   is allowed and efficient; otherwise it emits a separate
  ///                   fmul and fadd, which rounds twice and gives a different
  ///                   number. Both outcomes are legal, so the value is not
  ///                   fully determined by the IR.
  ///
  /// That decides what may be folded when all three operands are known:
  ///   strict                     -> a single point.
  ///   non-strict, roundings equal -> still a single point.
  ///   non-strict, roundings differ -> the hull of both. Proving equality to
  ///     either candidate would be unsound, because we do not know which one
  ///     the backend picks.
  ///
  /// With any uncertainty the outward-rounded interval hull is sound for both:
  /// every candidate result is a float lying inside the exact range, and
  /// rounding to nearest cannot escape an enclosure whose bounds are floats.
  void exec_float_fma(ar::CallBase* call, bool strict) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 3);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    if (!ret.is_floating_point_var()) {
      return;
    }

    const ScalarLit& la = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& lb = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& lc = this->_lit_factory.get_scalar(call->argument(2));

    auto ia = this->fp_interval(la);
    auto ib = this->fp_interval(lb);
    auto ic = this->fp_interval(lc);

    if (!ia.has_value() || !ib.has_value() || !ic.has_value()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    if (ia->is_point() && ib->is_point() && ic->is_point()) {
      const FloatingPoint& fa = ia->lo;
      const FloatingPoint& fb = ib->lo;
      const FloatingPoint& fc = ic->lo;
      if (fa.is_modeled() && fb.is_modeled() && fc.is_modeled() &&
          fa.bit_width() == fb.bit_width() &&
          fa.bit_width() == fc.bit_width()) {
        this->fold_fma_candidates(ret.var(), fa, fb, fc, strict);
        return;
      }
      // Unmodeled or mismatched width: stay unknown.
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }

    core::floating_point::Interval prod =
        core::floating_point::interval_bin_op('*', *ia, *ib);
    core::floating_point::Interval res =
        core::floating_point::interval_bin_op('+', prod, *ic);

    // Defensive: a point reaching here would be a two-rounding artefact rather
    // than a value we can justify, so refuse it.
    if (res.is_empty() || res.is_point()) {
      this->_inv.normal().float_assign_nondet(ret.var());
      return;
    }
    this->_inv.normal().float_assign_interval(ret.var(), res);
  }

  /// \brief Record the outcome of a fused multiply-add of three known values.
  ///
  /// Two candidates must be evaluated, and how they are built matters:
  ///
  ///   fused   = round(exact(a*b) + c)       -- one rounding
  ///   unfused = fl(fl(exact(a*b)) + c)     -- two roundings
  ///
  /// The unfused candidate MUST be built one operation at a time. Writing
  /// `a*b + c` in this file lets the compiler that builds IKOS contract it into
  /// a single hardware fma, making the "unfused" candidate bit-identical to
  /// the fused one and silently defeating the very distinction this function
  /// exists to draw -- observed directly: with the naive expression both
  /// candidates came back with identical bits on a case where the two roundings
  /// genuinely differ. The two round_op calls below each perform a single
  /// operation, so there is nothing for the compiler to fuse.
  ///
  /// Under a strict fma only `fused` is possible. Under fmuladd either one is,
  /// so the honest state is the hull of the two, which makes exact equality to
  /// either candidate unprovable whenever they differ.
  ///
  /// Candidates are compared by bit pattern, not with `==`: that keeps NaN and
  /// the signed zeros explicit, and keeps -Wfloat-equal honest.
  void fold_fma_candidates(Variable* var, const FloatingPoint& a,
                          const FloatingPoint& b, const FloatingPoint& c,
                          bool strict) {
    using core::floating_point::Interval;

    // One rounding: the host's correctly-rounded fused operation.
    FloatingPoint fused =
        (a.bit_width() == 32)
            ? FloatingPoint::from_float(
                  std::fmaf(a.to_float(), b.to_float(), c.to_float()))
            : FloatingPoint::from_double(
                  std::fma(a.to_double(), b.to_double(), c.to_double()));

    // Two roundings: separate operations, so nothing can be contracted.
    FloatingPoint prod = core::floating_point::round_op(a, b, '*', FE_TONEAREST);
    FloatingPoint unfused = core::floating_point::round_op(prod, c, '+',
                                                         FE_TONEAREST);

    bool f_nan = fused.is_nan();
    bool u_nan = unfused.is_nan();

    if (strict || fused.bits() == unfused.bits()) {
      this->_inv.normal().float_assign_cst(var, fused);
      return;
    }
    if (f_nan && u_nan) {
      this->_inv.normal().float_assign_cst(var, fused);
      return;
    }
    if (f_nan) {
      // Only the unfused candidate is ordered; NaN rides along in may_nan.
      this->_inv.normal().float_assign_interval(
          var, Interval{unfused, unfused, true});
      return;
    }
    if (u_nan) {
      this->_inv.normal().float_assign_interval(
          var, Interval{fused, fused, true});
      return;
    }
    // Order the two by numeric value; `<` is fine, it is `==` that is unsafe.
    bool f_lower = fused.to_double() < unfused.to_double();
    const FloatingPoint& lo = f_lower ? fused : unfused;
    const FloatingPoint& hi = f_lower ? unfused : fused;
    this->_inv.normal().float_assign_interval(var, Interval{lo, hi, false});
  }

  /// \brief Map an AR floating point predicate onto the core IEEE predicate
  static core::IEEEPredicate ieee_predicate(ar::Comparison::Predicate pred) {
    switch (pred) {
    case ar::Comparison::FOEQ:
      return core::IEEEPredicate::OEQ;
    case ar::Comparison::FOGT:
      return core::IEEEPredicate::OGT;
    case ar::Comparison::FOGE:
      return core::IEEEPredicate::OGE;
    case ar::Comparison::FOLT:
      return core::IEEEPredicate::OLT;
    case ar::Comparison::FOLE:
      return core::IEEEPredicate::OLE;
    case ar::Comparison::FONE:
      return core::IEEEPredicate::ONE;
    case ar::Comparison::FORD:
      return core::IEEEPredicate::ORD;
    case ar::Comparison::FUNO:
      return core::IEEEPredicate::UNO;
    case ar::Comparison::FUEQ:
      return core::IEEEPredicate::UEQ;
    case ar::Comparison::FUGT:
      return core::IEEEPredicate::UGT;
    case ar::Comparison::FUGE:
      return core::IEEEPredicate::UGE;
    case ar::Comparison::FULT:
      return core::IEEEPredicate::ULT;
    case ar::Comparison::FULE:
      return core::IEEEPredicate::ULE;
    case ar::Comparison::FUNE:
      return core::IEEEPredicate::UNE;
    default:
      ikos_unreachable("not a floating point comparison predicate");
    }
  }

  /// \brief Execute a floating point binary operation
  ///
  /// Interval arithmetic: each operand resolves to a range (a constant is a
  /// degenerate range) and the result range is computed with outward rounding.
  /// FRem is deliberately never folded -- LLVM's frem follows C's truncated
  /// remainder while the IEEE `remainder` operation rounds to nearest, and a
  /// wrong constant is worse than no constant.
  void exec_float_bin_operation(const ScalarLit& lhs,
                                ar::BinaryOperation::Operator op,
                                const ScalarLit& left,
                                const ScalarLit& right) {
    ikos_assert_msg(lhs.is_floating_point_var(),
                    "left hand side is not a floating point variable");

    if (left.is_floating_point_var()) {
      this->_inv.normal().uninit_assert_initialized(left.var());
    }
    if (right.is_floating_point_var()) {
      this->_inv.normal().uninit_assert_initialized(right.var());
    }

    char op_char = '\0';
    switch (op) {
    case ar::BinaryOperation::FAdd:
      op_char = '+';
      break;
    case ar::BinaryOperation::FSub:
      op_char = '-';
      break;
    case ar::BinaryOperation::FMul:
      op_char = '*';
      break;
    case ar::BinaryOperation::FDiv:
      op_char = '/';
      break;
    default:
      break;
    }

    auto li = this->fp_interval(left);
    auto ri = this->fp_interval(right);

    // LLVM's frem is C's fmod: the truncated remainder, whose sign follows the
    // dividend. It does not fit the interval_bin_op model (fmod is not
    // monotonic), so it gets its own path rather than a new op_char.
    if (op == ar::BinaryOperation::FRem) {
      this->exec_float_frem(lhs, li, ri);
      return;
    }

    if (op_char == '\0' || !li.has_value() || !ri.has_value()) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }

    // Same-variable identities. Interval arithmetic treats the two operands as
    // independent ranges, so `x - x` over x in [1,2] widens to [-1,1] even
    // though IEEE-754 makes it exactly +0. Where the semantics pin the result
    // down, hand back the constant instead of the widened hull.
    if (left.is_floating_point_var() && right.is_floating_point_var() &&
        left.var() == right.var() && !li->may_nan) {
      // pos_inf() carries the 64-bit encoding; comparing through to_double()
      // works whatever width the bounds were stored at.
      double pinf = core::floating_point::Interval::pos_inf().to_double();
      bool finite = li->lo.to_double() > -pinf && li->hi.to_double() < pinf;

      auto at_width = [](double v, uint16_t w)
          -> std::optional< FloatingPoint > {
        if (w == 32) {
          return FloatingPoint::from_float(static_cast<float>(v));
        }
        if (w == 64) {
          return FloatingPoint::from_double(v);
        }
        return std::nullopt;
      };

      // Subtraction and division need finite operands: inf - inf and inf / inf
      // are NaN, so the identities below do not hold at the infinities. Squaring
      // does not: |x| is monotonic, inf * inf is inf, and the straddle case
      // yields [0, +inf], which already contains every x^2. Letting `*` through
      // is what makes `x*x >= 0` provable for an unbounded, non-NaN x.
      if (finite || op_char == '*') {
        auto ty = ar::cast< ar::FloatType >(lhs.var()->type());
        uint16_t w = static_cast<uint16_t>(ty->bit_width());

        if (op_char == '-') {
          // x - x is exactly +0 for every finite x, in every rounding mode.
          if (auto c = at_width(0.0, w)) {
            this->_inv.normal().float_assign_cst(lhs.var(), *c);
            return;
          }
        } else if (op_char == '/' && finite &&
                   !(li->lo.to_double() <= 0.0 && li->hi.to_double() >= 0.0)) {
          // x / x is exactly 1 as long as x is not zero; 0/0 and inf/inf are NaN.
          if (auto c = at_width(1.0, w)) {
            this->_inv.normal().float_assign_cst(lhs.var(), *c);
            return;
          }
        } else if (op_char == '*') {
          // Squaring. fl(x*x) is monotonic in |x| -- the real product is, and
          // rounding preserves order -- so the image is attained at an endpoint.
          // That endpoint is itself a representable value and the program squares
          // it the same way, so no outward rounding is needed.
          //
          // Plain interval multiplication treats the two factors as independent
          // and returns [-4, 4] for x in [-2, 2], losing the fact that a square
          // is never negative. The true image is [0, 4].
          //
          // Square AT the target width. Squaring in double and narrowing would
          // be double rounding, which can differ from what a binary32 program
          // actually computes.
          auto sq = [](double v, uint16_t ww) -> double {
            if (ww == 32) {
              float f = static_cast< float >(v);
              return static_cast< double >(f * f);
            }
            return v * v;
          };

          double a = li->lo.to_double();
          double b = li->hi.to_double();
          double qa = sq(a, w);
          double qb = sq(b, w);
          double rlo;
          double rhi;
          if (a >= 0.0) {
            rlo = qa;
            rhi = qb;
          } else if (b <= 0.0) {
            rlo = qb;
            rhi = qa;
          } else {
            // Straddles zero, so zero is in the interval and is the minimum.
            rlo = 0.0;
            rhi = qa > qb ? qa : qb;
          }
          auto clo = at_width(rlo, w);
          auto chi = at_width(rhi, w);
          if (clo.has_value() && chi.has_value()) {
            this->_inv.normal().float_assign_interval(
                lhs.var(), core::floating_point::Interval{*clo, *chi, false});
            return;
          }
        }
      }
    }

    core::floating_point::Interval res =
        core::floating_point::interval_bin_op(op_char, *li, *ri);
    if (res.is_empty()) {
      this->_inv.normal().float_assign_nondet(lhs.var());
      return;
    }
    this->_inv.normal().float_assign_interval(lhs.var(), res);
  }

  /// \brief Execute a vector binary operation
  void exec_vector_bin_operation(ar::BinaryOperation* s) {
    const AggregateLit& lhs = this->_lit_factory.get_aggregate(s->result());
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    // Ignore the semantic while being sound
    Variable* ptr = this->init_aggregate_memory(lhs);
    this->_inv.normal().mem_forget_reachable(ptr);
  }

public:
  /// \brief Execute a Comparison statement
  void exec(ar::Comparison* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const ScalarLit& left = this->_lit_factory.get_scalar(s->left());
    const ScalarLit& right = this->_lit_factory.get_scalar(s->right());

    switch (s->predicate()) {
      case ar::Comparison::UIEQ:
      case ar::Comparison::SIEQ: {
        this->exec_int_comparison(IntPredicate::EQ, left, right);
      } break;
      case ar::Comparison::UINE:
      case ar::Comparison::SINE: {
        this->exec_int_comparison(IntPredicate::NE, left, right);
      } break;
      case ar::Comparison::UIGT:
      case ar::Comparison::SIGT: {
        this->exec_int_comparison(IntPredicate::GT, left, right);
      } break;
      case ar::Comparison::UIGE:
      case ar::Comparison::SIGE: {
        this->exec_int_comparison(IntPredicate::GE, left, right);
      } break;
      case ar::Comparison::UILT:
      case ar::Comparison::SILT: {
        this->exec_int_comparison(IntPredicate::LT, left, right);
      } break;
      case ar::Comparison::UILE:
      case ar::Comparison::SILE: {
        this->exec_int_comparison(IntPredicate::LE, left, right);
      } break;
      case ar::Comparison::FOEQ:
      case ar::Comparison::FOGT:
      case ar::Comparison::FOGE:
      case ar::Comparison::FOLT:
      case ar::Comparison::FOLE:
      case ar::Comparison::FONE:
      case ar::Comparison::FORD:
      case ar::Comparison::FUNO:
      case ar::Comparison::FUEQ:
      case ar::Comparison::FUGT:
      case ar::Comparison::FUGE:
      case ar::Comparison::FULT:
      case ar::Comparison::FULE:
      case ar::Comparison::FUNE: {
        this->exec_float_comparison(s->predicate(), left, right);
      } break;
      case ar::Comparison::PEQ: {
        this->exec_ptr_comparison(PointerPredicate::EQ, left, right);
      } break;
      case ar::Comparison::PNE: {
        this->exec_ptr_comparison(PointerPredicate::NE, left, right);
      } break;
      case ar::Comparison::PGT: {
        this->exec_ptr_comparison(PointerPredicate::GT, left, right);
      } break;
      case ar::Comparison::PGE: {
        this->exec_ptr_comparison(PointerPredicate::GE, left, right);
      } break;
      case ar::Comparison::PLT: {
        this->exec_ptr_comparison(PointerPredicate::LT, left, right);
      } break;
      case ar::Comparison::PLE: {
        this->exec_ptr_comparison(PointerPredicate::LE, left, right);
      } break;
      default: {
        ikos_unreachable("unreachable");
      }
    }
  }

private:
  /// \brief Execute an integer comparison
  void exec_int_comparison(IntPredicate pred,
                           const ScalarLit& left,
                           const ScalarLit& right) {
    if (left.is_machine_int()) {
      if (right.is_machine_int()) {
        if (!compare(pred, left.machine_int(), right.machine_int())) {
          this->_inv.set_normal_flow_to_bottom();
        }
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().int_add(pred, left.machine_int(), right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else if (left.is_machine_int_var()) {
      if (right.is_machine_int()) {
        this->_inv.normal().int_add(pred, left.var(), right.machine_int());
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().int_add(pred, left.var(), right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else {
      ikos_unreachable("unexpected left operand");
    }
  }

  /// \brief Execute a floating point comparison
  ///
  /// The frontend lowers a comparison into two branches, one assuming the
  /// predicate and one assuming its inverse, assigning 1 or 0 to the result
  /// variable accordingly. Refining the path here is therefore what makes the
  /// result variable a known constant: if the predicate is definitely false the
  /// branch becomes infeasible, and the surviving branch carries the value.
  ///
  /// When the predicate might or might not hold we keep both branches feasible and
  /// instead narrow the operand's recorded interval, which is what lets later
  /// assertions such as `x >= 1 && x <= 2 => x + 1 <= 3` go through.
  void exec_float_comparison(ar::Comparison::Predicate predicate,
                             const ScalarLit& left,
                             const ScalarLit& right) {
    if (left.is_floating_point_var()) {
      this->_inv.normal().uninit_assert_initialized(left.var());
    }
    if (right.is_floating_point_var()) {
      this->_inv.normal().uninit_assert_initialized(right.var());
    }

    auto li = this->fp_interval(left);
    auto ri = this->fp_interval(right);
    if (!li.has_value() || !ri.has_value()) {
      return;
    }

    core::IEEEPredicate pred = ieee_predicate(predicate);

    // Same-variable comparison: `x pred x` carries exact NaN semantics that do
    // not depend on the ordered range, and the var-vs-constant refinement below
    // cannot express them. This is what makes isnan() work. How isnan() gets
    // here is platform-dependent: Darwin lowers it to `fcmp une x, x`, while
    // glibc lowers it to `llvm.is.fpclass.f64(x, 3)` and the frontend rewrites
    // that into `fcmp uno x, x` (see translate_intrinsic_call).
    //
    // Derived from the IEEE-754 spellings, where `u<pred>` = unordered OR pred
    // and `o<pred>` = ordered AND pred, together with the facts that x==x, x>=x
    // and x<=x all hold whenever x is ordered, while x<x and x>x never do:
    //
    //   une, uno, ult, ugt  <=>  x IS NaN
    //   ord, oeq, oge, ole  <=>  x is NOT NaN
    //   one, olt, ogt       =  always false   (needs ordered and x != x)
    //   ueq, ule, uge       =  always true   (ordered operands satisfy them)
    if (left.is_floating_point_var() && right.is_floating_point_var() &&
        left.var() == right.var()) {
      this->refine_same_var_nan(pred, left.var());
      return;
    }

    switch (core::floating_point::interval_holds(pred, *li, *ri)) {
    case core::floating_point::Holds::No:
      this->_inv.set_normal_flow_to_bottom();
      break;
    case core::floating_point::Holds::Yes:
      break;
    case core::floating_point::Holds::Might: {
      // Narrow whichever side is the variable. Two variables is a relation an
      // interval domain cannot express, so that case is left alone.
      if (left.is_floating_point_var() && right.is_floating_point()) {
        this->_inv.normal().float_add(pred, left.var(), right.floating_point());
      }
      else if (left.is_floating_point() && right.is_floating_point_var()) {
        this->_inv.normal().float_add(pred, left.floating_point(), right.var());
      }
      break;
    }
    }
  }

  /// \brief Constrain `x` from a comparison of `x` against itself.
  ///
  /// See the table at the call site. Only +/-infinity and the current bounds are
  /// ever written, and +/-infinity is exactly representable at every float width,
  /// so this cannot mislabel a narrow value as a wider one.
  void refine_same_var_nan(core::IEEEPredicate pred, Variable* x) {
    using Interval = core::floating_point::Interval;

    switch (pred) {
    case core::IEEEPredicate::UNE:
    case core::IEEEPredicate::UNO:
    case core::IEEEPredicate::ULT:
    case core::IEEEPredicate::UGT: {
      // Only NaN satisfies this. If x is already known ordered, the branch is
      // unreachable rather than merely imprecise.
      const Interval* cur = this->_inv.normal().float_get_interval(x);
      if (cur != nullptr && !cur->may_nan) {
        this->_inv.set_normal_flow_to_bottom();
      }
      else {
        // Empty ordered range with NaN possible == definitely NaN.
        this->_inv.normal().float_assign_interval(
            x, Interval{Interval::pos_inf(), Interval::neg_inf(), true});
      }
    } break;

    case core::IEEEPredicate::ORD:
    case core::IEEEPredicate::OEQ:
    case core::IEEEPredicate::OGE:
    case core::IEEEPredicate::OLE: {
      // Requires x to be ordered: keep the range, drop the NaN possibility.
      const Interval* cur = this->_inv.normal().float_get_interval(x);
      Interval iv = (cur != nullptr) ? *cur : Interval::top_value();
      iv.may_nan = false;
      if (iv.ordered_empty()) {
        this->_inv.set_normal_flow_to_bottom();
      }
      else {
        this->_inv.normal().float_assign_interval(x, iv);
      }
    } break;

    case core::IEEEPredicate::ONE:
    case core::IEEEPredicate::OLT:
    case core::IEEEPredicate::OGT:
      // Ordered and x != x, or x < x / x > x: never true.
      this->_inv.set_normal_flow_to_bottom();
      break;

    case core::IEEEPredicate::UEQ:
    case core::IEEEPredicate::ULE:
    case core::IEEEPredicate::UGE:
      // Satisfied by ordered operands, so always true. Nothing to constrain.
      break;
    }
  }

  /// \brief Execute a pointer comparison
  void exec_ptr_comparison(PointerPredicate pred,
                           const ScalarLit& left,
                           const ScalarLit& right) {
    if (left.is_null()) {
      if (right.is_null()) {
        // Compare `null pred null`
        if (pred == PointerPredicate::NE || pred == PointerPredicate::GT ||
            pred == PointerPredicate::LT) {
          this->_inv.set_normal_flow_to_bottom();
        }
      } else if (right.is_pointer_var()) {
        // Compare `null pred p`
        this->refine_addresses(right.var());

        if (pred == PointerPredicate::EQ) {
          this->_inv.normal().nullity_assert_null(right.var());
        } else if (pred == PointerPredicate::NE ||
                   pred == PointerPredicate::GT ||
                   pred == PointerPredicate::LT) {
          this->_inv.normal().nullity_assert_non_null(right.var());
        } else {
          this->_inv.normal().uninit_assert_initialized(right.var());
        }
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else if (left.is_pointer_var()) {
      if (right.is_null()) {
        // Compare `p pred null`
        this->refine_addresses(left.var());

        if (pred == PointerPredicate::EQ) {
          this->_inv.normal().nullity_assert_null(left.var());
        } else if (pred == PointerPredicate::NE ||
                   pred == PointerPredicate::GT ||
                   pred == PointerPredicate::LT) {
          this->_inv.normal().nullity_assert_non_null(left.var());
        } else {
          this->_inv.normal().uninit_assert_initialized(left.var());
        }
      } else if (right.is_pointer_var()) {
        // Compare `p pred q`

        // Reduction with the external pointer analysis
        this->refine_addresses_offset(left.var());
        this->refine_addresses_offset(right.var());

        this->_inv.normal().pointer_add(pred, left.var(), right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else {
      ikos_unreachable("unexpected left operand");
    }
  }

public:
  /// \brief Execute an Unreachable statement
  void exec(ar::Unreachable*) override {
    // Unreachable propagates exceptions
    this->_inv.set_normal_flow_to_bottom();
  }

  /// \brief Execute an Allocate statement
  void exec(ar::Allocate* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& array_size =
        this->_lit_factory.get_scalar(s->array_size());
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    // Allocate the memory
    MemoryLocation* addr = this->_mem_factory.get_local(s->result());
    this->allocate_memory(lhs.var(),
                          addr,
                          Nullity::non_null(),
                          Lifetime::allocated(),
                          MemoryInitialValue::Uninitialized);

    if (this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
      // Set the allocation size variable
      Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
      auto element_size = MachineInt(this->_data_layout.alloc_size_in_bytes(
                                         s->allocated_type()),
                                     this->_data_layout.pointers.bit_width,
                                     Unsigned);
      if (array_size.is_machine_int()) {
        bool overflow;
        MachineInt alloc_size_int =
            mul(array_size.machine_int(), element_size, overflow);
        if (overflow) {
          this->_inv.set_normal_flow_to_bottom(); // undefined behavior
        } else {
          this->_inv.normal().int_assign(alloc_size_var, alloc_size_int);

          // When the size of the allocation is known (like it is here)
          // we mark the storage as uninitialized by assigning it
          // the undefined value.
          this->_inv.normal().mem_write(lhs.var(),
                                        ScalarLit::undefined(),
                                        alloc_size_int);
        }
      } else if (array_size.is_machine_int_var()) {
        this->_inv.normal().int_apply(IntBinaryOperator::MulNoWrap,
                                      alloc_size_var,
                                      array_size.var(),
                                      element_size);
      } else {
        ikos_unreachable("unexpected array size parameter");
      }
    }
  }

  /// \brief Execute a PointerShift statement
  void exec(ar::PointerShift* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& base = this->_lit_factory.get_scalar(s->pointer());
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");
    ikos_assert_msg(base.is_null() || base.is_pointer_var(),
                    "unexpected base operand");

    // Build a linear expression of the operands
    uint64_t bit_width = this->_data_layout.pointers.bit_width;
    auto zero = MachineInt::zero(bit_width, Unsigned);
    auto offset_expr = IntLinearExpression(zero);

    for (auto it = s->term_begin(), et = s->term_end(); it != et; ++it) {
      auto term = *it;
      const ScalarLit& offset = this->_lit_factory.get_scalar(term.second);

      if (offset.is_machine_int()) {
        offset_expr.add(
            mul(term.first, offset.machine_int().cast(bit_width, Unsigned)));
      } else if (offset.is_machine_int_var()) {
        offset_expr.add(term.first, offset.var());
      } else {
        ikos_unreachable("unexpected offset operand");
      }
    }

    if (base.is_null()) {
      this->_inv.normal().pointer_assign(lhs.var(),
                                         this->_mem_factory.get_absolute_zero(),
                                         Nullity::null());
      this->_inv.normal().pointer_assign(lhs.var(), lhs.var(), offset_expr);
    } else {
      this->_inv.normal().pointer_assign(lhs.var(), base.var(), offset_expr);
    }

    this->normalize_absolute_zero_nullity(lhs.var());
  }

  /// \brief Execute a Load statement
  ///
  /// Reading uninitialized memory is an error.
  void exec(ar::Load* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(s->operand());

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    const Literal& result = this->_lit_factory.get(s->result());

    auto size =
        MachineInt(this->_data_layout.store_size_in_bytes(s->result()->type()),
                   this->_data_layout.pointers.bit_width,
                   Unsigned);

    if (result.is_scalar()) {
      const ScalarLit& lhs = result.scalar();
      ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

      if (!s->is_volatile()) {
        // Perform memory read in the value domain
        this->_inv.normal().mem_read(lhs, ptr.var(), size);
      } else {
        this->_inv.normal().scalar_assign_nondet(lhs.var());
      }

      // Reduction between value and pointer analysis
      if (lhs.is_pointer_var()) {
        this->refine_addresses_offset(lhs.var());
      }
    } else if (result.is_aggregate()) {
      const AggregateLit& lhs = result.aggregate();
      ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

      Variable* lhs_ptr = this->init_aggregate_memory(lhs);

      if (!s->is_volatile()) {
        // Perform memory read in the value domain
        this->_inv.normal().mem_copy(lhs_ptr,
                                     ptr.var(),
                                     ScalarLit::machine_int(size));
      } else {
        this->_inv.normal().mem_forget_reachable(lhs_ptr);
      }
    } else {
      ikos_unreachable("unexpected left hand side");
    }
  }

  /// \brief Execute a Store statement
  ///
  /// Writing an uninitialized variable is an error.
  void exec(ar::Store* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(s->pointer());

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore memory write, analysis could be unsound.
      // See CheckKind::IgnoredStore
      return;
    }

    const Literal& val = this->_lit_factory.get(s->value());

    auto size =
        MachineInt(this->_data_layout.store_size_in_bytes(s->value()->type()),
                   this->_data_layout.pointers.bit_width,
                   Unsigned);

    if (val.is_scalar()) {
      const ScalarLit& rhs = val.scalar();

      if (rhs.is_pointer_var()) {
        this->refine_addresses_offset(rhs.var());
      }

      // Perform memory write in the value domain
      this->_inv.normal().mem_write(ptr.var(), rhs, size);
    } else if (val.is_aggregate()) {
      this->mem_write_aggregate(ptr.var(), val.aggregate());
    } else {
      ikos_unreachable("unexpected right hand side");
    }
  }

  /// \brief Execute an ExtractElement statement
  void exec(ar::ExtractElement* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const Literal& lhs = this->_lit_factory.get(s->result());
    const AggregateLit& rhs = this->_lit_factory.get_aggregate(s->aggregate());
    const ScalarLit& offset = this->_lit_factory.get_scalar(s->offset());
    ikos_assert_msg(rhs.is_var(), "right hand side is not a variable");

    Variable* rhs_ptr = this->aggregate_pointer(rhs);
    Variable* read_ptr =
        this->_var_factory.get_named_shadow(this->void_ptr_type(),
                                            "shadow.extract_element.ptr");
    if (offset.is_machine_int_var()) {
      this->_inv.normal().pointer_assign(read_ptr, rhs_ptr, offset.var());
    } else if (offset.is_machine_int()) {
      this->_inv.normal().pointer_assign(read_ptr,
                                         rhs_ptr,
                                         offset.machine_int());
    } else {
      ikos_unreachable("unexpected offset operand");
    }

    auto size =
        MachineInt(this->_data_layout.store_size_in_bytes(s->result()->type()),
                   this->_data_layout.pointers.bit_width,
                   Unsigned);

    if (lhs.is_scalar()) {
      ikos_assert_msg(lhs.scalar().is_var(),
                      "left hand side is not a variable");

      this->_inv.normal().mem_read(lhs.scalar(), read_ptr, size);
    } else if (lhs.is_aggregate()) {
      ikos_assert_msg(lhs.aggregate().is_var(),
                      "left hand side is not a variable");

      Variable* lhs_ptr = this->init_aggregate_memory(lhs.aggregate());
      this->_inv.normal().mem_copy(lhs_ptr,
                                   read_ptr,
                                   ScalarLit::machine_int(size));
    } else {
      ikos_unreachable("unexpected left hand side");
    }

    // Clean-up
    this->_inv.normal().pointer_forget(read_ptr);
  }

  /// \brief Execute an InsertElement statement
  ///
  /// Unlike most statements, this accepts undefined aggregate operands
  void exec(ar::InsertElement* s) override {
    if (s->offset()->is_undefined_constant() ||
        s->element()->is_undefined_constant()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const AggregateLit& lhs = this->_lit_factory.get_aggregate(s->result());
    const AggregateLit& rhs = this->_lit_factory.get_aggregate(s->aggregate());
    const ScalarLit& offset = this->_lit_factory.get_scalar(s->offset());
    const Literal& element = this->_lit_factory.get(s->element());
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    Variable* lhs_ptr = this->init_aggregate_memory(lhs);

    // First, copy the aggregate value
    this->mem_write_aggregate(lhs_ptr, rhs);

    // Then, insert the element
    Variable* write_ptr =
        this->_var_factory.get_named_shadow(this->void_ptr_type(),
                                            "shadow.insert_element.ptr");
    if (offset.is_machine_int_var()) {
      this->_inv.normal().pointer_assign(write_ptr, lhs_ptr, offset.var());
    } else if (offset.is_machine_int()) {
      this->_inv.normal().pointer_assign(write_ptr,
                                         lhs_ptr,
                                         offset.machine_int());
    } else {
      ikos_unreachable("unexpected offset operand");
    }

    auto size =
        MachineInt(this->_data_layout.store_size_in_bytes(s->element()->type()),
                   this->_data_layout.pointers.bit_width,
                   Unsigned);

    if (element.is_scalar()) {
      this->_inv.normal().mem_write(write_ptr, element.scalar(), size);
    } else if (element.is_aggregate()) {
      this->mem_write_aggregate(write_ptr, element.aggregate());
    } else {
      ikos_unreachable("unexpected element operand");
    }

    // Clean-up
    this->_inv.normal().pointer_forget(write_ptr);
  }

  /// \brief Execute a ShuffleVector statement
  void exec(ar::ShuffleVector* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->init_global_operands(s);

    const AggregateLit& lhs = this->_lit_factory.get_aggregate(s->result());
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    // Ignore the semantic while being sound
    Variable* ptr = this->init_aggregate_memory(lhs);
    this->_inv.normal().mem_forget_reachable(ptr);
  }

  /// \brief Execute a LandingPad statement
  void exec(ar::LandingPad*) override {}

  /// \brief Execute a Resume statement
  void exec(ar::Resume* s) override {
    if (s->has_undefined_constant_operand()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    this->_inv.resume_exception();
  }

  /// @}
  /// \name Execute call statements
  /// @{

  /// \brief Execute a call to the given extern function
  void exec_extern_call(ar::CallBase* call, ar::Function* fun) override {
    ikos_assert(fun->is_declaration());
    ikos_assert(ar::TypeVerifier::is_valid_call(call, fun->type()));

    if (fun->is_intrinsic()) {
      this->exec_intrinsic_call(call, fun->intrinsic_id());
    } else {
      this->exec_unknown_extern_call(call);
    }
  }

  /// \brief Execute a call to the given intrinsic function
  void exec_intrinsic_call(ar::CallBase* call, ar::Intrinsic::ID id) override {
    this->_inv.normal().normalize();

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    if (id == ar::Intrinsic::IkosPartitioningVar) {
      // Unlike most functions, it propagates uninitialized variables
      this->exec_ikos_partitioning_var(call);
      return;
    }

    // Check for uninitialized variables
    for (auto it = call->op_begin(), et = call->op_end(); it != et; ++it) {
      ar::Value* op = *it;

      if (isa< ar::UndefinedConstant >(op)) {
        this->_inv.set_normal_flow_to_bottom();
        return;
      } else if (auto iv = dyn_cast< ar::InternalVariable >(op)) {
        Variable* var = this->_var_factory.get_internal(iv);
        this->_inv.normal().uninit_assert_initialized(var);
      }
    }

    this->_inv.normal().normalize();

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    switch (id) {
      case ar::Intrinsic::MemoryCopy:
      case ar::Intrinsic::MemoryMove: {
        this->exec_memcpy_or_memmove(call);
      } break;
      case ar::Intrinsic::MemorySet: {
        this->exec_memset(call);
      } break;
      case ar::Intrinsic::VarArgStart:
      case ar::Intrinsic::VarArgEnd:
      case ar::Intrinsic::VarArgGet:
      case ar::Intrinsic::VarArgCopy: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ true,
                                /* ignore_unknown_write = */ true,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::StackSave:
      case ar::Intrinsic::StackRestore: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LifetimeStart:
      case ar::Intrinsic::LifetimeEnd: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::EhTypeidFor: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::Trap: {
        this->exec_abort(call);
      } break;
      case ar::Intrinsic::FloatFmuladd: {
        this->exec_float_fma(call, /* strict = */ false);
      } break;
      case ar::Intrinsic::FloatFma: {
        this->exec_float_fma(call, /* strict = */ true);
      } break;
      case ar::Intrinsic::FloatAbs: {
        this->exec_float_abs(call);
      } break;
      case ar::Intrinsic::FloatMaxnum: {
        this->exec_float_maxmin(call, /* is_max = */ true);
      } break;
      case ar::Intrinsic::FloatMinnum: {
        this->exec_float_maxmin(call, /* is_max = */ false);
      } break;
      case ar::Intrinsic::FloatFloor: {
        this->exec_float_round(call, RoundMode::Floor);
      } break;
      case ar::Intrinsic::FloatCeil: {
        this->exec_float_round(call, RoundMode::Ceil);
      } break;
      case ar::Intrinsic::FloatTrunc: {
        this->exec_float_round(call, RoundMode::Trunc);
      } break;
      case ar::Intrinsic::FloatSqrt: {
        this->exec_float_sqrt(call);
      } break;
      case ar::Intrinsic::FloatLog: {
        this->exec_float_log(call, LogBase::Nat);
      } break;
      case ar::Intrinsic::FloatLog2: {
        this->exec_float_log(call, LogBase::Two);
      } break;
      case ar::Intrinsic::FloatLog10: {
        this->exec_float_log(call, LogBase::Ten);
      } break;
      case ar::Intrinsic::FloatPow: {
        this->exec_float_pow(call);
      } break;
      case ar::Intrinsic::FloatRound: {
        this->exec_float_round(call, RoundMode::HalfAway);
      } break;
      case ar::Intrinsic::FloatRint: {
        this->exec_float_round(call, RoundMode::HalfEven);
      } break;
      case ar::Intrinsic::FloatCopysign: {
        this->exec_float_copysign(call);
      } break;
      // <ikos/analyzer/intrinsic.h>
      case ar::Intrinsic::IkosAssert:
      case ar::Intrinsic::IkosAssume:
      case ar::Intrinsic::IkosNonDet: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::IkosCounterInit: {
        this->exec_ikos_counter_init(call);
      } break;
      case ar::Intrinsic::IkosCounterIncr: {
        this->exec_ikos_counter_incr(call);
      } break;
      case ar::Intrinsic::IkosCheckMemAccess:
      case ar::Intrinsic::IkosCheckStringAccess: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::IkosAssumeMemSize: {
        this->exec_ikos_assume_mem_size(call);
      } break;
      case ar::Intrinsic::IkosForgetMemory: {
        this->exec_ikos_forget_memory(call);
      } break;
      case ar::Intrinsic::IkosAbstractMemory: {
        this->exec_ikos_abstract_memory(call);
      } break;
      case ar::Intrinsic::IkosWatchMemory: {
        this->exec_ikos_watch_memory(call);
      } break;
      case ar::Intrinsic::IkosPartitioningVar: {
        this->exec_ikos_partitioning_var(call);
      } break;
      case ar::Intrinsic::IkosPartitioningJoin: {
        this->exec_ikos_partitioning_join(call);
      } break;
      case ar::Intrinsic::IkosPartitioningDisable: {
        this->exec_ikos_partitioning_disable(call);
      } break;
      case ar::Intrinsic::IkosPrintInvariant:
      case ar::Intrinsic::IkosPrintValues: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      // <stdlib.h>
      case ar::Intrinsic::LibcMalloc: {
        this->exec_malloc(call);
      } break;
      case ar::Intrinsic::LibcCalloc: {
        this->exec_calloc(call);
      } break;
      case ar::Intrinsic::LibcValloc: {
        this->exec_valloc(call);
      } break;
      case ar::Intrinsic::LibcAlignedAlloc: {
        this->exec_aligned_alloc(call);
      } break;
      case ar::Intrinsic::LibcRealloc: {
        this->exec_realloc(call);
      } break;
      case ar::Intrinsic::LibcFree: {
        this->exec_free(call);
      } break;
      case ar::Intrinsic::LibcAbs: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcRand:
      case ar::Intrinsic::LibcSrand: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcExit: {
        this->exec_exit(call);
      } break;
      case ar::Intrinsic::LibcAbort: {
        this->exec_abort(call);
      } break;
      // <errno.h>
      case ar::Intrinsic::LibcErrnoLocation: {
        this->exec_errno_location(call);
      } break;
      // <fcntl.h>
      case ar::Intrinsic::LibcOpen: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      // <unistd.h>
      case ar::Intrinsic::LibcClose: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcRead: {
        this->exec_read(call);
      } break;
      case ar::Intrinsic::LibcWrite: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      // <stdio.h>
      case ar::Intrinsic::LibcGets: {
        this->exec_gets(call);
      } break;
      case ar::Intrinsic::LibcFgets: {
        this->exec_fgets(call);
      } break;
      case ar::Intrinsic::LibcGetc: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcFgetc: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcGetchar: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcPuts: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcFputs: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcPutc: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcFputc: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcPrintf: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcFprintf: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcSprintf: {
        this->exec_sprintf(call);
      } break;
      case ar::Intrinsic::LibcSnprintf: {
        this->exec_snprintf(call);
      } break;
      case ar::Intrinsic::LibcScanf: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ true,
                                /* ignore_unknown_write = */ true,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcFscanf: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ true,
                                /* ignore_unknown_write = */ true,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcSscanf: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ true,
                                /* ignore_unknown_write = */ true,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcFopen: {
        this->exec_fopen(call);
      } break;
      case ar::Intrinsic::LibcFclose: {
        this->exec_fclose(call);
      } break;
      case ar::Intrinsic::LibcFflush: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      // <string.h>
      case ar::Intrinsic::LibcStrlen: {
        this->exec_strlen(call);
      } break;
      case ar::Intrinsic::LibcStrnlen: {
        this->exec_strnlen(call);
      } break;
      case ar::Intrinsic::LibcStrcpy: {
        this->exec_strcpy(call);
      } break;
      case ar::Intrinsic::LibcStrncpy: {
        this->exec_strncpy(call);
      } break;
      case ar::Intrinsic::LibcStrcat: {
        this->exec_strcat(call);
      } break;
      case ar::Intrinsic::LibcStrncat: {
        this->exec_strncat(call);
      } break;
      case ar::Intrinsic::LibcStrcmp: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcStrncmp: {
        this->exec_unknown_call(call,
                                /* may_write_params = */ false,
                                /* ignore_unknown_write = */ false,
                                /* may_write_globals = */ false,
                                /* may_throw_exc = */ false);
      } break;
      case ar::Intrinsic::LibcStrstr: {
        this->exec_strstr(call);
      } break;
      case ar::Intrinsic::LibcStrchr: {
        this->exec_strchr(call);
      } break;
      case ar::Intrinsic::LibcStrdup: {
        this->exec_strdup(call);
      } break;
      case ar::Intrinsic::LibcStrndup: {
        this->exec_strndup(call);
      } break;
      case ar::Intrinsic::LibcStrcpyCheck: {
        this->exec_strcpy(call);
      } break;
      case ar::Intrinsic::LibcMemoryCopyCheck: {
        this->exec_memcpy_or_memmove(call);
      } break;
      case ar::Intrinsic::LibcMemoryMoveCheck: {
        this->exec_memcpy_or_memmove(call);
      } break;
      case ar::Intrinsic::LibcMemorySetCheck: {
        this->exec_memset(call);
      } break;
      case ar::Intrinsic::LibcStrcatCheck: {
        this->exec_strcat(call);
      } break;
      case ar::Intrinsic::LibcppNew:
      case ar::Intrinsic::LibcppNewArray: {
        this->exec_new(call);
      } break;
      case ar::Intrinsic::LibcppDelete: {
        this->exec_free(call);
      } break;
      case ar::Intrinsic::LibcppDeleteArray: {
        // TODO(marthaud): delete[] also calls the destructor on each element
        this->exec_free(call);
      } break;
      case ar::Intrinsic::LibcppAllocateException: {
        this->exec_allocate_exception(call);
      } break;
      case ar::Intrinsic::LibcppFreeException: {
        this->exec_free(call);
      } break;
      case ar::Intrinsic::LibcppThrow: {
        this->exec_throw(call);
      } break;
      case ar::Intrinsic::LibcppBeginCatch: {
        this->exec_begin_catch(call);
      } break;
      case ar::Intrinsic::LibcppEndCatch: {
        this->exec_end_catch(call);
      } break;
      default: {
        ikos_unreachable("unreachable");
      } break;
    }
  }

  /// \brief Execute a call to an unknown extern function
  void exec_unknown_extern_call(ar::CallBase* call) override {
    this->exec_unknown_call(call,
                            /* may_write_params = */ true,
                            /* ignore_unknown_write = */ true,
                            /* may_write_globals = */ false,
                            /* may_throw_exc = */ true);
  }

  /// \brief Execute a call to an unknown internal function
  void exec_unknown_intern_call(ar::CallBase* call) override {
    this->exec_unknown_call(call,
                            /* may_write_params = */ true,
                            /* ignore_unknown_write = */ false,
                            /* may_write_globals = */ true,
                            /* may_throw_exc = */ true);
  }

  /// \brief Execute a call to an unknown function
  ///
  /// \param call
  ///   The call statement
  /// \param may_write_params
  ///   True if the function call might write on a pointer parameter
  /// \param ignore_unknown_write
  ///   True to ignore writes on unknown pointer parameters (unsound)
  /// \param may_write_globals
  ///   True if the function call might update a global variable
  /// \param may_throw_exc
  ///   True if the function call might throw an exception
  void exec_unknown_call(ar::CallBase* call,
                         bool may_write_params,
                         bool ignore_unknown_write,
                         bool may_write_globals,
                         bool may_throw_exc) override {
    this->_inv.normal().normalize();

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    // Check for uninitialized variables
    for (auto it = call->op_begin(), et = call->op_end(); it != et; ++it) {
      ar::Value* op = *it;

      if (isa< ar::UndefinedConstant >(op)) {
        this->_inv.set_normal_flow_to_bottom();
        return;
      } else if (auto iv = dyn_cast< ar::InternalVariable >(op)) {
        Variable* var = this->_var_factory.get_internal(iv);
        this->_inv.normal().uninit_assert_initialized(var);
      }
    }

    this->_inv.normal().normalize();

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    if (may_write_globals) {
      // Forget all memory contents
      this->_inv.normal().mem_forget_all();
    } else if (may_write_params) {
      // Forget all memory contents pointed by pointer parameters
      for (auto it = call->arg_begin(), et = call->arg_end(); it != et; ++it) {
        ar::Value* arg = *it;

        if (!isa< ar::InternalVariable >(arg) ||
            !isa< ar::PointerType >(arg->type())) {
          continue;
        }

        auto iv = cast< ar::InternalVariable >(arg);
        Variable* ptr = this->_var_factory.get_internal(iv);

        this->init_global_operand(arg);
        this->refine_addresses(ptr);

        if (this->_inv.normal().nullity_is_null(ptr)) {
          continue; // Safe
        } else if (ignore_unknown_write &&
                   this->_inv.normal().pointer_to_points_to(ptr).is_top()) {
          // Ignore side effect on the memory
          // See CheckKind::IgnoredCallSideEffectOnPointerParameter
          continue;
        } else {
          this->_inv.normal().mem_forget_reachable(ptr);
        }
      }
    }

    if (may_throw_exc) {
      // The call can throw exceptions
      this->throw_unknown_exceptions();
    }

    // ASSUMPTION:
    // The claim about the correctness of the program under analysis can be
    // made only if all calls to unavailable code are assumed to be correct
    // and without side-effects. We will assume that the lhs of an external
    // call site is always initialized. However, in case of a pointer, we do
    // not assume that a non-null pointer is returned.
    if (call->has_result()) {
      // Forget the result
      const Literal& ret = this->_lit_factory.get(call->result());

      if (ret.is_scalar()) {
        ikos_assert_msg(ret.scalar().is_var(),
                        "left hand side is not a variable");
        this->_inv.normal().scalar_assign_nondet(ret.scalar().var());
      } else if (ret.is_aggregate()) {
        ikos_assert_msg(ret.aggregate().is_var(),
                        "left hand side is not a variable");
        Variable* ret_ptr = this->aggregate_pointer(ret.aggregate());
        this->_inv.normal().mem_forget_reachable(ret_ptr);
      } else {
        ikos_unreachable("unexpected left hand side");
      }
    }
  }

private:
  /// @}
  /// \name Execution of intrinsic functions
  /// @{

  /// \brief Execute a call to memcpy(dest, src, len) or memmove(dest, src, len)
  void exec_memcpy_or_memmove(ar::CallBase* call) {
    this->init_global_operands(call);

    // Both src and dest must be already allocated in memory so offsets and
    // sizes for both src and dest are already part of the invariants
    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& src = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    if (!this->prepare_mem_access(src)) {
      return;
    }
    if (!this->prepare_mem_access(dest)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(dest.var()).is_top()) {
      // Ignore memory copy/move, analysis could be unsound.
      // See CheckKind::IgnoredMemoryCopy, CheckKind::IgnoredMemoryMove
    } else if (cast< ar::IntegerConstant >(call->argument(5))->value() == 0) {
      // Non-volatile
      this->_inv.normal().mem_copy(dest.var(), src.var(), size);
    } else {
      // Volatile
      if (size.is_machine_int()) {
        this->_inv.normal().mem_forget_reachable(dest.var(),
                                                 size.machine_int());
      } else if (size.is_machine_int_var()) {
        IntInterval size_intv = this->_inv.normal().int_to_interval(size.var());
        this->_inv.normal().mem_forget_reachable(dest.var(), size_intv.ub());
      } else {
        ikos_unreachable("unreachable");
      }
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->assign(lhs, dest);
    }
  }

  /// \brief Execute a call to memset(dest, byte, len)
  void exec_memset(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& value = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    ikos_assert_msg(value.is_machine_int_var() || value.is_machine_int(),
                    "unexpected value operand");
    ikos_assert_msg(size.is_machine_int_var() || size.is_machine_int(),
                    "unexpected size operand");

    if (!this->prepare_mem_access(dest)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(dest.var()).is_top()) {
      // Ignore memory set, analysis could be unsound.
      // See CheckKind::IgnoredMemorySet
    } else {
      this->_inv.normal().mem_set(dest.var(), value, size);
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->assign(lhs, dest);
    }
  }

  /// \brief Execute a call to ikos.counter.init
  void exec_ikos_counter_init(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 1);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& init = this->_lit_factory.get_scalar(call->argument(0));

    ikos_assert_msg(ret.is_machine_int_var(),
                    "left hand side is not an integer variable");
    ikos_assert_msg(init.is_machine_int(), "operand is not a machine integer");

    this->_inv.normal().counter_init(ret.var(), init.machine_int());
  }

  /// \brief Execute a call to ikos.counter.incr
  void exec_ikos_counter_incr(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 2);
    ikos_assert(call->result() == call->argument(0));

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& incr = this->_lit_factory.get_scalar(call->argument(1));

    ikos_assert_msg(ret.is_machine_int_var(),
                    "left hand side is not an integer variable");
    ikos_assert_msg(incr.is_machine_int(), "operand is not a machine integer");

    this->_inv.normal().counter_incr(ret.var(), incr.machine_int());
  }

  /// \brief Execute a call to ikos.assume_mem_size
  void exec_ikos_assume_mem_size(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (!this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
      return;
    }

    PointsToSet addrs = this->_inv.normal().pointer_to_points_to(ptr.var());

    if (addrs.is_bottom()) {
      return;
    } else if (addrs.is_top()) {
      // Ignore (this is sound)
      return;
    }

    for (auto addr : addrs) {
      Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);

      if (size.is_machine_int()) {
        this->_inv.normal().int_assign(alloc_size_var, size.machine_int());
      } else if (size.is_machine_int_var()) {
        this->_inv.normal().int_assign(alloc_size_var, size.var());
      } else {
        ikos_unreachable("unreachable");
      }
    }
  }

  /// \brief Execute a call to ikos.forget_memory
  void exec_ikos_forget_memory(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore ikos.forget_memory, analysis could be unsound.
      // See CheckKind::UnknownMemoryAccess
    } else if (size.is_machine_int()) {
      this->_inv.normal().mem_forget_reachable(ptr.var(), size.machine_int());
    } else if (size.is_machine_int_var()) {
      IntInterval size_intv = this->_inv.normal().int_to_interval(size.var());
      this->_inv.normal().mem_forget_reachable(ptr.var(), size_intv.ub());
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute a call to ikos.abstract_memory
  void exec_ikos_abstract_memory(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore ikos.abstract_memory, analysis could be unsound.
      // See CheckKind::UnknownMemoryAccess
    } else if (size.is_machine_int()) {
      this->_inv.normal().mem_abstract_reachable(ptr.var(), size.machine_int());
    } else if (size.is_machine_int_var()) {
      IntInterval size_intv = this->_inv.normal().int_to_interval(size.var());
      this->_inv.normal().mem_abstract_reachable(ptr.var(), size_intv.ub());
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute a call to ikos.watch_memory
  void exec_ikos_watch_memory(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    // Save the watched pointer
    Variable* watch_mem_ptr =
        this->_var_factory.get_named_shadow(this->void_ptr_type(),
                                            "shadow.watch_mem.ptr");
    this->_inv.normal().pointer_assign(watch_mem_ptr, ptr.var());

    // Save the watched size
    Variable* watch_mem_size =
        this->_var_factory.get_named_shadow(ar::IntegerType::size_type(
                                                this->_ctx.bundle),
                                            "shadow.watch_mem.size");
    if (size.is_machine_int()) {
      this->_inv.normal().int_assign(watch_mem_size, size.machine_int());
    } else if (size.is_machine_int_var()) {
      this->_inv.normal().int_assign(watch_mem_size, size.var());
    } else {
      ikos_unreachable("unexpected size parameter");
    }
  }

  /// \brief Execute a call to ikos.partitioning.var.si32
  void exec_ikos_partitioning_var(ar::CallBase* call) {
    const ScalarLit& arg = this->_lit_factory.get_scalar(call->argument(0));

    if (arg.is_var()) {
      this->_inv.normal().partitioning_set_variable(arg.var());
    }
  }

  /// \brief Execute a call to ikos.partitioning.join
  void exec_ikos_partitioning_join(ar::CallBase*) {
    this->_inv.normal().partitioning_join();
  }

  /// \brief Execute a call to ikos.partitioning.disable
  void exec_ikos_partitioning_disable(ar::CallBase*) {
    this->_inv.normal().partitioning_disable();
  }

  /// \brief Execute a dynamic allocation
  void exec_dynamic_alloc(ar::CallBase* call,
                          ar::Value* size,
                          bool may_return_null,
                          bool may_throw_exc,
                          MemoryInitialValue init_val) {
    if (may_throw_exc) {
      this->throw_unknown_exceptions();
    }

    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& size_l = this->_lit_factory.get_scalar(size);
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    auto nullity = may_return_null ? Nullity::top() : Nullity::non_null();

    MemoryLocation* addr =
        this->_mem_factory.get_dyn_alloc(call, this->_call_context);

    if (size_l.is_machine_int_var()) {
      this->allocate_memory(lhs.var(),
                            addr,
                            nullity,
                            Lifetime::allocated(),
                            init_val,
                            size_l.var());
    } else if (size_l.is_machine_int()) {
      this->allocate_memory(lhs.var(),
                            addr,
                            nullity,
                            Lifetime::allocated(),
                            init_val,
                            size_l.machine_int());
    } else {
      ikos_unreachable("unexpected size operand");
    }
  }

  /// \brief Execute a call to libc malloc
  ///
  /// #include <stdlib.h>
  /// void* malloc(size_t size);
  ///
  /// The malloc() function returns a pointer to a newly allocated block size
  /// bytes long, or a null pointer if the block could not be allocated.
  void exec_malloc(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ true,
                             /* may_throw_exc = */ false,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a call to libc calloc
  ///
  /// #include <stdlib.h>
  /// void* calloc(size_t count, size_t size);
  ///
  /// The calloc() function contiguously allocates enough space for count
  /// objects that are size bytes of memory each and returns a pointer to the
  /// allocated memory. The allocated memory is filled with bytes of value zero.
  void exec_calloc(ar::CallBase* call) {
    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& count = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    // Allocate the memory
    MemoryLocation* addr =
        this->_mem_factory.get_dyn_alloc(call, this->_call_context);
    this->allocate_memory(lhs.var(),
                          addr,
                          Nullity::top(),
                          Lifetime::allocated(),
                          MemoryInitialValue::Zero);

    if (!this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
      return;
    }

    // Set the allocation size variable
    Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
    if (count.is_machine_int()) {
      if (size.is_machine_int()) {
        bool overflow;
        MachineInt alloc_size_int =
            mul(count.machine_int(), size.machine_int(), overflow);
        if (overflow) {
          this->_inv.set_normal_flow_to_bottom(); // Undefined behavior
        } else {
          this->_inv.normal().int_assign(alloc_size_var, alloc_size_int);
        }
      } else if (size.is_machine_int_var()) {
        this->_inv.normal().int_apply(IntBinaryOperator::MulNoWrap,
                                      alloc_size_var,
                                      count.machine_int(),
                                      size.var());
      } else {
        ikos_unreachable("unexpected size parameter");
      }
    } else if (count.is_machine_int_var()) {
      if (size.is_machine_int()) {
        this->_inv.normal().int_apply(IntBinaryOperator::MulNoWrap,
                                      alloc_size_var,
                                      count.var(),
                                      size.machine_int());
      } else if (size.is_machine_int_var()) {
        this->_inv.normal().int_apply(IntBinaryOperator::MulNoWrap,
                                      alloc_size_var,
                                      count.var(),
                                      size.var());
      } else {
        ikos_unreachable("unexpected size parameter");
      }
    } else {
      ikos_unreachable("unexpected count parameter");
    }
  }

  /// \brief Execute a call to libc valloc
  ///
  /// #include <stdlib.h>
  /// void* valloc(size_t size);
  ///
  /// The valloc() function allocates size bytes of memory and returns a pointer
  /// to the allocated memory.  The allocated memory is aligned on a page
  /// boundary.
  void exec_valloc(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ true,
                             /* may_throw_exc = */ false,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a call to libc aligned_alloc
  ///
  /// #include <stdlib.h>
  /// void* aligned_alloc(size_t alignment, size_t size);
  ///
  /// The function posix_memalign() allocates size bytes and places the address
  /// of the allocated memory in *memptr. The address of the allocated memory
  /// will be a multiple of alignment, which must be a power of two and a
  /// multiple of sizeof(void *). If size is 0, then posix_memalign() returns
  /// either NULL, or a unique pointer value that can later be successfully
  /// passed to free(3).
  ///
  /// The function aligned_alloc() is the same as memalign(), except for the
  /// added restriction that size should be a multiple of alignment.
  void exec_aligned_alloc(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(1),
                             /* may_return_null = */ true,
                             /* may_throw_exc = */ false,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a call to libc realloc
  ///
  /// #include <stdlib.h>
  /// void* realloc(void* ptr, size_t size);
  ///
  /// The realloc() function tries to change the size of the allocation pointed
  /// to by ptr to size, and returns ptr.  If there is not enough room to
  /// enlarge the memory allocation pointed to by ptr, realloc() creates a new
  /// allocation, copies as much of the old data pointed to by ptr as will fit
  /// to the new allocation, frees the old allocation, and returns a pointer to
  /// the allocated memory.  If ptr is NULL, realloc() is identical to a call to
  /// malloc() for size bytes.  If size is zero and ptr is not NULL, a new,
  /// minimum sized object is allocated and the original object is freed.  When
  /// extending a region allocated with calloc(3), realloc(3) does not guarantee
  /// that the additional memory is also zero-filled.
  void exec_realloc(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    // Allocate the memory
    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");

      MemoryLocation* addr =
          this->_mem_factory.get_dyn_alloc(call, this->_call_context);
      if (size.is_machine_int_var()) {
        this->allocate_memory(lhs.var(),
                              addr,
                              Nullity::top(),
                              Lifetime::allocated(),
                              MemoryInitialValue::Uninitialized,
                              size.var());
      } else if (size.is_machine_int()) {
        this->allocate_memory(lhs.var(),
                              addr,
                              Nullity::top(),
                              Lifetime::allocated(),
                              MemoryInitialValue::Uninitialized,
                              size.machine_int());
      } else {
        ikos_unreachable("unexpected size operand");
      }
    }

    // Copy data
    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");

      if (ptr.is_pointer_var() &&
          !this->_inv.normal().nullity_is_null(ptr.var())) {
        // This should be the size of `ptr` instead of `size`
        this->_inv.normal().mem_copy(lhs.var(), ptr.var(), size);
      }
    }

    this->_inv.normal().normalize();

    if (this->_inv.is_normal_flow_bottom()) {
      // If the mem_copy generated an error
      return;
    }

    // Free the pointer
    this->exec_free(call);
  }

  /// \brief Execute a call to libc free, libc++ delete or delete[], etc.
  ///
  /// #include <stdlib.h>
  /// void free(void* ptr);
  ///
  /// The free() function deallocates the memory allocation pointed to by ptr.
  /// If ptr is a NULL pointer, no operation is performed.
  void exec_free(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));

    if (ptr.is_null()) {
      // This is safe, according to C standards
      return;
    }

    ikos_assert_msg(ptr.is_pointer_var(), "unexpected parameter");

    if (this->_inv.normal().nullity_is_null(ptr.var())) {
      // This is safe, according to C standards
      return;
    }

    // Reduction between value and pointer analysis
    this->refine_addresses(ptr.var());

    PointsToSet addrs = this->_inv.normal().pointer_to_points_to(ptr.var());

    if (addrs.is_bottom()) {
      return;
    } else if (addrs.is_top()) {
      // Ignored memory deallocation, analysis could be unsound.
      // See CheckKind::IgnoredFree
      return;
    }

    // Forget memory contents
    this->_inv.normal().mem_forget_reachable(ptr.var());

    // Forget the allocation size and set the new lifetime
    for (auto addr : addrs) {
      if (!isa< DynAllocMemoryLocation >(addr)) {
        if (addrs.size() == 1) {
          // This is an error
          this->_inv.set_normal_flow_to_bottom();
          return;
        } else {
          continue;
        }
      }

      if (addrs.size() == 1) {
        this->_inv.normal().lifetime_assign_deallocated(addr);
      } else {
        this->_inv.normal().lifetime_forget(addr);
      }

      if (this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
        Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
        this->_inv.normal().int_forget(alloc_size_var);
      }
    }
  }

  /// \brief Execute a call to libc exit
  ///
  /// #include <stdlib.h>
  /// void exit(int status);
  ///
  /// The exit() functions terminate a process.
  ///
  /// It also performs the following functions in the order listed:
  ///   1. Call the functions registered with the atexit(3) function, in the
  /// reverse order of their registration.
  ///   2. Flush all open output streams.
  ///   3. Close all open streams.
  ///   4. Unlink all files created with the tmpfile(3) function.
  void exec_exit(ar::CallBase* /*call*/) {
    // TODO(marthaud): analyze functions registered by atexit()
    this->_inv.set_normal_flow_to_bottom();
  }

  /// \brief Execute a call to libc abort
  ///
  /// #include <stdlib.h>
  /// void abort(void);
  ///
  /// The abort() function causes abnormal program termination to occur, unless
  /// the signal SIGABRT is being caught and the signal handler does not return.
  void exec_abort(ar::CallBase* /*call*/) {
    this->_inv.set_normal_flow_to_bottom();
  }

  /// \brief Execute a call to libc errno_location
  ///
  /// #include <errno.h>
  /// int* __errno_location(void);
  ///
  /// The __errno_location() function returns a pointer to the errno variable.
  void exec_errno_location(ar::CallBase* call) {
    // Forget the current value of errno
    MemoryLocation* addr = this->_mem_factory.get_libc_errno();
    this->_inv.normal().mem_forget(addr);

    // Assign the result
    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->_inv.normal().pointer_assign(lhs.var(), addr, Nullity::non_null());
    }
  }

  /// \brief Execute a call to libc read
  ///
  /// #include <unistd.h>
  /// ssize_t read(int handle, void* buffer, size_t nbyte);
  ///
  /// The read() function attempts to read nbytes from the file associated with
  /// handle, and places the characters read into buffer. If the file is opened
  /// using O_TEXT, it removes carriage returns and detects the end of the file.
  ///
  /// The function returns the number of bytes read. On end-of-file, 0 is
  /// returned, on error it returns -1, setting errno to indicate the type of
  /// error that occurred.
  void exec_read(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore read, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else if (size.is_machine_int()) {
      this->_inv.normal().mem_abstract_reachable(ptr.var(), size.machine_int());
    } else if (size.is_machine_int_var()) {
      IntInterval size_intv = this->_inv.normal().int_to_interval(size.var());
      this->_inv.normal().mem_abstract_reachable(ptr.var(), size_intv.ub());
    } else {
      ikos_unreachable("unreachable");
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_machine_int_var(),
                      "left hand side is not an integer variable");
      this->_inv.normal().int_assign_nondet(lhs.var());
    }
  }

  /// \brief Execute a call to libc gets
  ///
  /// #include <stdio.h>
  /// char* gets(char* str);
  ///
  /// The gets() function is equivalent to fgets() with an infinite size and a
  /// stream of stdin, except that the newline character (if any) is not stored
  /// in the string.  It is the caller's responsibility to ensure that the input
  /// line, if any, is sufficiently short to fit in the string.
  void exec_gets(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore gets, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else {
      this->_inv.normal().mem_abstract_reachable(ptr.var());
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->_inv.normal().pointer_assign(lhs.var(), ptr.var());
      this->_inv.normal().nullity_set(lhs.var(),
                                      Nullity::top()); // Returns null on errors
    }
  }

  /// \brief Execute a call to libc fgets
  ///
  /// #include <stdio.h>
  /// char* fgets(char* str, int size, FILE* stream);
  ///
  /// The fgets() function reads at most one less than the number of characters
  /// specified by size from the given stream and stores them in the string str.
  /// Reading stops when a newline character is found, at end-of-file or error.
  /// The newline, if any, is retained.  If any characters are read and there is
  /// no error, a `\0' character is appended to end the string.
  void exec_fgets(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    // Size is a ui32, convert it to a size_t
    auto size_type = ar::IntegerType::size_type(this->_ctx.bundle);

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore fgets, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else if (size.is_machine_int()) {
      this->_inv.normal()
          .mem_abstract_reachable(ptr.var(),
                                  size.machine_int()
                                      .cast(size_type->bit_width(),
                                            ar::Unsigned));
    } else if (size.is_machine_int_var()) {
      IntInterval size_intv = this->_inv.normal()
                                  .int_to_interval(size.var())
                                  .cast(size_type->bit_width(), ar::Unsigned);
      this->_inv.normal().mem_abstract_reachable(ptr.var(), size_intv.ub());
    } else {
      ikos_unreachable("unreachable");
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->_inv.normal().pointer_assign(lhs.var(), ptr.var());
      this->_inv.normal().nullity_set(lhs.var(),
                                      Nullity::top()); // Returns null on errors
    }
  }

  /// \brief Execute a call to libc sprintf
  ///
  /// #include <stdio.h>
  /// int sprintf(char* str, const char* format, ...);
  ///
  /// The snprintf() and vsnprintf() functions will write at most size-1 of the
  /// characters printed into the output string (the size'th character then gets
  /// the terminating `\0'); if the return value is greater than or equal to the
  /// size argument, the string was too short and some of the printed characters
  /// were discarded.  The output is always null-terminated, unless size is 0.
  ///
  /// The sprintf() and vsprintf() functions effectively assume a size of
  /// INT_MAX + 1.
  void exec_sprintf(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
      // Ignore sprintf, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else {
      this->_inv.normal().mem_abstract_reachable(ptr.var());
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_machine_int_var(),
                      "left hand side is not an integer variable");
      this->_inv.normal().int_assign_nondet(lhs.var());
    }
  }

  /// \brief Execute a call to libc snprintf
  ///
  /// #include <stdio.h>
  /// int snprintf(char* str, size_t size, const char* format, ...);
  ///
  /// The snprintf() and vsnprintf() functions will write at most size-1 of the
  /// characters printed into the output string (the size'th character then gets
  /// the terminating `\0'); if the return value is greater than or equal to the
  /// size argument, the string was too short and some of the printed characters
  /// were discarded.  The output is always null-terminated, unless size is 0.
  void exec_snprintf(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));

    // Calling snprintf with zero bufsz and null pointer buffer can be used to
    // determine the buffer size needed to contain the output. That case is
    // allowed. Otherwise, check first argument.
    bool checkPointer =
        !(size.is_machine_int() && size.machine_int().is_zero());

    if (checkPointer) {
      if (!this->prepare_mem_access(ptr)) {
        return;
      }

      if (this->_inv.normal().pointer_to_points_to(ptr.var()).is_top()) {
        // Ignore snprintf, analysis could be unsound.
        // See CheckKind::IgnoredCallSideEffectOnPointerParameter
      } else if (size.is_machine_int()) {
        this->_inv.normal().mem_abstract_reachable(ptr.var(),
                                                   size.machine_int());
      } else if (size.is_machine_int_var()) {
        IntInterval size_intv = this->_inv.normal().int_to_interval(size.var());
        this->_inv.normal().mem_abstract_reachable(ptr.var(), size_intv.ub());
      } else {
        ikos_unreachable("unreachable");
      }
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_machine_int_var(),
                      "left hand side is not an integer variable");
      this->_inv.normal().int_assign_nondet(lhs.var());
    }
  }

  /// \brief Execute a call to libc fopen
  ///
  /// #include <stdio.h>
  /// FILE* fopen(const char* path, const char* mode);
  ///
  /// The fopen() function opens the file whose name is the string pointed to by
  /// path and associates a stream with it.
  void exec_fopen(ar::CallBase* call) {
    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    MemoryLocation* addr =
        this->_mem_factory.get_dyn_alloc(call, this->_call_context);

    this->allocate_memory(lhs.var(),
                          addr,
                          Nullity::top(),
                          Lifetime::allocated(),
                          MemoryInitialValue::Unknown);
  }

  /// \brief Execute a call to libc fclose
  ///
  /// #include <stdio.h>
  /// int fclose(FILE* stream);
  ///
  /// The fclose() function dissociates the named stream from its underlying
  /// file or set of functions.  If the stream was being used for output, any
  /// buffered data is written first, using fflush(3).
  void exec_fclose(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));

    // fclose(NULL) is undefined behavior
    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    this->exec_free(call);

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_machine_int_var(),
                      "left hand side is not an integer variable");
      this->_inv.normal().int_assign_nondet(lhs.var());
    }
  }

  /// \brief Execute a call to libc strlen
  ///
  /// #include <string.h>
  /// size_t strlen(const char* s);
  ///
  /// The strlen() function computes the length of the string s.
  ///
  /// The strlen() function returns the number of characters that precede the
  /// terminating NULL character.
  void exec_strlen(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& str = this->_lit_factory.get_scalar(call->argument(0));

    if (!this->prepare_mem_access(str)) {
      return;
    }

    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    // lhs is in [0, size - 1]
    this->_inv.normal().int_assign_nondet(lhs.var());

    PointsToSet addrs = this->_inv.normal().pointer_to_points_to(str.var());

    if (addrs.is_top()) {
      return;
    }

    boost::optional< AbstractDomain > inv = boost::none;

    for (MemoryLocation* addr : addrs) {
      AbstractDomain tmp = this->_inv;

      if (auto gv = dyn_cast< GlobalMemoryLocation >(addr)) {
        auto alloc_size = MachineInt(this->_data_layout.store_size_in_bytes(
                                         gv->global_var()->type()->pointee()),
                                     this->_data_layout.pointers.bit_width,
                                     Unsigned);
        tmp.normal().int_add(IntPredicate::LT, lhs.var(), alloc_size);
      } else {
        Variable* size_var = this->_var_factory.get_alloc_size(addr);
        tmp.normal().int_add(IntPredicate::LT, lhs.var(), size_var);
      }

      if (!inv) {
        inv = std::move(tmp);
      } else {
        inv->join_with(tmp);
      }
    }

    if (!inv) {
      this->_inv.set_to_bottom();
    } else {
      this->_inv = std::move(*inv);
    }
  }

  /// \brief Execute a call to libc strlen
  ///
  /// #include <string.h>
  /// size_t strnlen(const char* s, size_t maxlen);
  ///
  /// The strnlen() function attempts to compute the length of s, but never
  /// scans beyond the first maxlen bytes of s.
  ///
  /// The strnlen() function returns either the same result as strlen() or
  /// maxlen, whichever is smaller.
  void exec_strnlen(ar::CallBase* call) {
    this->exec_strlen(call);

    if (!call->has_result()) {
      return;
    }

    // lhs <= maxlen
    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& maxlen = this->_lit_factory.get_scalar(call->argument(1));
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (maxlen.is_machine_int()) {
      this->_inv.normal().int_add(IntPredicate::LE,
                                  lhs.var(),
                                  maxlen.machine_int());
    } else if (maxlen.is_machine_int_var()) {
      this->_inv.normal().int_add(IntPredicate::LE, lhs.var(), maxlen.var());
    } else {
      ikos_unreachable("unexpected maxlen parameter");
    }
  }

  /// \brief Execute a call to libc strcpy
  ///
  /// #include <string.h>
  /// char* strcpy(char* dst, const char* src);
  ///
  /// The strcpy() function copies the string src to dst (including the
  /// terminating `\0' character).
  ///
  /// The strcpy() function returns dst.
  void exec_strcpy(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& src = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(dest)) {
      return;
    }
    if (!this->prepare_mem_access(src)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(dest.var()).is_top()) {
      // Ignore strcpy, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else {
      // Do not keep track of the content
      this->_inv.normal().mem_abstract_reachable(dest.var());
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->assign(lhs, dest);
    }
  }

  /// \brief Execute a call to libc strncpy
  ///
  /// #include <string.h>
  /// char* strncpy(char* dst, const char* src, size_t n);
  ///
  /// The strncpy() function copies at most n characters from src into dst.
  /// If src is less than n characters long, the remainder of dst is filled with
  /// `\0' characters. Otherwise, dst is not terminated.
  ///
  /// The strncpy() function returns dst.
  void exec_strncpy(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& src = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    if (!this->prepare_mem_access(dest)) {
      return;
    }
    if (!this->prepare_mem_access(src)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(dest.var()).is_top()) {
      // Ignore strncpy, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else if (size.is_machine_int()) {
      this->_inv.normal().mem_abstract_reachable(dest.var(),
                                                 size.machine_int());
    } else if (size.is_machine_int_var()) {
      IntInterval size_intv = this->_inv.normal().int_to_interval(size.var());
      this->_inv.normal().mem_abstract_reachable(dest.var(), size_intv.ub());
    } else {
      ikos_unreachable("unreachable");
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->assign(lhs, dest);
    }
  }

  /// \brief Execute a call to libc strcat
  ///
  /// #include <string.h>
  /// char* strcat(char* s1, const char* s2);
  ///
  /// The strcat() function appends a copy of the null-terminated string s2 to
  /// the end of the null-terminated string s1, then add a terminating \0. The
  /// string s1 must have sufficient space to hold the result.
  ///
  /// The strcat() function returns the pointer s1.
  void exec_strcat(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& s1 = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& s2 = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(s1)) {
      return;
    }
    if (!this->prepare_mem_access(s2)) {
      return;
    }

    if (this->_inv.normal().pointer_to_points_to(s1.var()).is_top()) {
      // Ignore strcat, analysis could be unsound.
      // See CheckKind::IgnoredCallSideEffectOnPointerParameter
    } else {
      // Do not keep track of the content
      this->_inv.normal().mem_abstract_reachable(s1.var());
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->assign(lhs, s1);
    }
  }

  /// \brief Execute a call to libc strcat
  ///
  /// #include <string.h>
  /// char* strncat(char* s1, const char* s2, size_t n);
  ///
  /// The strncat() function appends a copy of the null-terminated string s2 to
  /// the end of the null-terminated string s1, then add a terminating `\0'. The
  /// string s1 must have sufficient space to hold the result.
  ///
  /// The strncat() function appends not more than n characters from s2, and
  /// then adds a terminating `\0'.
  ///
  /// The strncat() function returns the pointer s1.
  void exec_strncat(ar::CallBase* call) { this->exec_strcat(call); }

  /// \brief Execute a call to libc strstr
  ///
  /// #include <string.h>
  /// char* strstr(const char* haystack, const char* needle);
  ///
  /// The strstr() function locates the first occurrence of the null-terminated
  /// string needle in the null-terminated string haystack.
  void exec_strstr(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& haystack =
        this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& needle = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(haystack)) {
      return;
    }
    if (!this->prepare_mem_access(needle)) {
      return;
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->_inv.normal().pointer_assign(lhs.var(), haystack.var());
      this->_inv.normal().nullity_set(lhs.var(), Nullity::top());
      this->_inv.normal().pointer_forget_offset(lhs.var());
    }
  }

  /// \brief Execute a call to libc strchr
  ///
  /// #include <string.h>
  /// char* strchr(const char* s, int c);
  ///
  /// The strchr() function locates the first occurrence of c (converted to a
  /// char) in the string pointed to by s.  The terminating null character is
  /// considered to be part of the string; therefore if c is `\0', the functions
  /// locate the terminating `\0'.
  void exec_strchr(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& s = this->_lit_factory.get_scalar(call->argument(0));

    if (!this->prepare_mem_access(s)) {
      return;
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");
      this->_inv.normal().pointer_assign(lhs.var(), s.var());
      this->_inv.normal().nullity_set(lhs.var(), Nullity::top());
      this->_inv.normal().pointer_forget_offset(lhs.var());
    }
  }

  /// \brief Execute a call to libc strdup
  ///
  /// #include <string.h>
  /// char* strdup(const char* s1);
  ///
  /// The strdup() function allocates sufficient memory for a copy of the string
  /// s1, does the copy, and returns a pointer to it.  The pointer may
  /// subsequently be used as an argument to the function free(3).
  void exec_strdup(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& s = this->_lit_factory.get_scalar(call->argument(0));

    if (!this->prepare_mem_access(s)) {
      return;
    }

    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_pointer_var(),
                      "left hand side is not a pointer variable");

      MemoryLocation* addr =
          this->_mem_factory.get_dyn_alloc(call, this->_call_context);
      this->allocate_memory(lhs.var(),
                            addr,
                            Nullity::top(),
                            Lifetime::allocated(),
                            MemoryInitialValue::Unknown);
    }
  }

  /// \brief Execute a call to libc strndup
  ///
  /// #include <string.h>
  /// char* strndup(const char* s1, size_t n);
  ///
  /// The strndup() function copies at most n characters from the string s1
  /// always NUL terminating the copied string.
  void exec_strndup(ar::CallBase* call) {
    this->init_global_operands(call);

    const ScalarLit& s = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& n = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(s)) {
      return;
    }

    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    MemoryLocation* addr =
        this->_mem_factory.get_dyn_alloc(call, this->_call_context);
    this->allocate_memory(lhs.var(),
                          addr,
                          Nullity::top(),
                          Lifetime::allocated(),
                          MemoryInitialValue::Unknown);

    if (this->_opts.test(ExecutionEngine::UpdateAllocSizeVar)) {
      // sizeof(addr) <= n
      Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);

      if (n.is_machine_int()) {
        this->_inv.normal().int_add(IntPredicate::LE,
                                    alloc_size_var,
                                    n.machine_int());
      } else if (n.is_machine_int_var()) {
        this->_inv.normal().int_add(IntPredicate::LE, alloc_size_var, n.var());
      } else {
        ikos_unreachable("unexpected size operand");
      }
    }
  }

  /// \brief Execute a call to libc++ new or new[]
  ///
  /// operator new(size_t)
  /// operator new[](size_t)
  ///
  /// Allocates requested number of bytes. These allocation functions are called
  /// by new-expressions to allocate memory in which new object would then be
  /// initialized. They may also be called using regular function call syntax.
  void exec_new(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ false,
                             /* may_throw_exc = */ true,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a call to libc++ allocate exception
  ///
  /// void* __cxa_allocate_exception(size_t thrown_size) noexcept;
  ///
  /// Allocates memory to hold the exception to be thrown. thrown_size is the
  /// size of the exception object. Can allocate additional memory to hold
  /// private data. If memory can not be allocated, call std::terminate().
  void exec_allocate_exception(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ false,
                             /* may_throw_exc = */ false,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a call to libc++ throw
  ///
  /// __cxa_throw(void* exception, std::type_info* tinfo, void (*dest)(void*))
  ///
  /// After constructing the exception object with the throw argument value, the
  /// generated code calls the __cxa_throw runtime library routine. This routine
  /// never returns.
  void exec_throw(ar::CallBase* /*call*/) { this->_inv.throw_exception(); }

  /// \brief Execute a call to libc++ begin catch
  ///
  /// void* __cxa_begin_catch(void* exc_obj) noexcept;
  ///
  /// When entering a catch scope, __cxa_begin_catch is called with the
  /// exception object `exc_obj`. This routine returns the adjusted pointer to
  /// the exception object.
  ///
  /// Assume that it returns `exc_obj` unchanged.
  void exec_begin_catch(ar::CallBase* call) {
    if (!call->has_result()) {
      return;
    }
    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& exc_obj = this->_lit_factory.get_scalar(call->argument(0));
    this->assign(lhs, exc_obj);
  }

  /// \brief Execute a call to libc++ end catch
  ///
  /// void __cxa_end_catch();
  ///
  /// Locates the most recently caught exception and decrements its handler
  /// count. Removes the exception from the caughtÃ“exception stack, if the
  /// handler count goes to zero. Destroys the exception if the handler count
  /// goes to zero, and the exception was not re-thrown by throw. Collaboration
  /// between __cxa_rethrow() and __cxa_end_catch() is necessary to handle the
  /// last point. Though implementation-defined, one possibility is for
  /// __cxa_rethrow() to set a flag in the handlerCount member of the exception
  /// header to mark an exception being rethrown.
  void exec_end_catch(ar::CallBase* /*call*/) {}

  /// @}

public:
  void match_down(ar::CallBase* call, ar::Function* called) override {
    ikos_assert(called->is_definition());
    ikos_assert(ar::TypeVerifier::is_valid_call(call, called->type()));

    auto param_it = called->param_begin();
    auto param_et = called->param_end();
    auto arg_it = call->arg_begin();
    auto arg_et = call->arg_end();
    for (; param_it != param_et && arg_it != arg_et; ++param_it, ++arg_it) {
      this->init_global_operand(*arg_it);
      this->implicit_bitcast(this->_lit_factory.get(*param_it),
                             this->_lit_factory.get(*arg_it));
    }
  }

  void match_up(ar::CallBase* call, ar::ReturnValue* ret) override {
    if (ret == nullptr || !ret->has_operand()) {
      // No return value
      return;
    }

    const Literal& return_value = this->_lit_factory.get(ret->operand());

    if (call->has_result()) {
      // Assign the result variable
      const Literal& result = this->_lit_factory.get(call->result());
      this->init_global_operand(ret->operand());
      this->implicit_bitcast(result, return_value);

      if (return_value.is_var()) {
        // If the current partitioning is based on the return variable,
        // we automatically update it to the result variable.
        auto partitioning_var = this->_inv.normal().partitioning_variable();
        if (partitioning_var && *partitioning_var == return_value.var()) {
          this->_inv.normal().partitioning_set_variable(result.var());
        }
      }
    }

    // Clean-up the invariant
    if (!return_value.is_var()) {
      return;
    } else if (return_value.is_scalar()) {
      this->_inv.normal().scalar_forget(return_value.var());
    } else if (return_value.is_aggregate()) {
      this->_inv.normal().mem_forget_reachable(return_value.var());
      this->_inv.normal().scalar_forget(return_value.var());
    } else {
      ikos_unreachable("unreachable");
    }
  }

}; // end class NumericalExecutionEngine

} // end namespace analyzer
} // end namespace ikos

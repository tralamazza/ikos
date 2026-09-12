/*******************************************************************************
 *
 * \file
 * \brief Translate a LLVM module and Debug Information into an AR bundle
 *
 * Author: Maxime Arthaud
 *         Nija Shi
 *         Arnaud Venet
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

#include <ikos/core/support/assert.hpp>
#include <iostream>

#include <ikos/ar/semantic/statement.hpp>

#include <ikos/frontend/llvm/import/exception.hpp>

#include "bundle.hpp"
#include "constant.hpp"
#include "function.hpp"
#include "library_function.hpp"
#include "type.hpp"

namespace ikos {
namespace frontend {
namespace import {

ar::GlobalVariable* BundleImporter::translate_global_variable(
    llvm::GlobalVariable* gv) {
  auto it = this->_globals.find(gv);

  if (it != this->_globals.end()) {
    return it->second;
  }

  // Build the ar::GlobalVariable

  std::string name;
  if (gv->hasName()) {
    name = gv->getName().str();
  } else {
    name = this->_bundle->find_available_name("__unnamed_global_var");
  }

  // Special names for intrinsic global variables (such as llvm.global_ctors)
  if (name.rfind("llvm.", 0) == 0) {
    name = "ar." + name.substr(5);
  }

  // With opaque pointers the pointee comes from GlobalVariable::getValueType().
  llvm::Type* pointee = gv->getValueType();

  // Extract the DWARF type from debug information
  llvm::SmallVector< llvm::DIGlobalVariableExpression*, 1 > dbgs;
  gv->getDebugInfo(dbgs);

  ar::Type* ar_pointee_type = nullptr;
  if (dbgs.empty()) {
    // No debug info

    // Prefer signed integers, because most global variables without
    // debug information are strings (const char*)
    ar_pointee_type = _ctx.type_imp->translate_type(pointee, ar::Signed);
  } else {
    // Use debug information to build the exact type
    llvm::DIGlobalVariable* di_gv = dbgs[0]->getVariable();
    auto di_type = llvm::cast_or_null< llvm::DIType >(di_gv->getRawType());

    try {
      ar_pointee_type = _ctx.type_imp->translate_type(pointee, di_type);
    } catch (const TypeDebugInfoMismatch&) {
      if (!this->_allow_debug_info_mismatch) {
        throw;
      }
      ar_pointee_type = _ctx.type_imp->translate_type(pointee, ar::Signed);
    }
  }

  // Create the ar::GlobalVariable
  ikos_assert(ar_pointee_type);
  ar::PointerType* ar_type =
      ar::PointerType::get(this->_context, ar_pointee_type);
  ar::GlobalVariable* ar_gv =
      ar::GlobalVariable::create(this->_bundle,
                                 ar_type,
                                 name,
                                 /*is_definition = */ !gv->isDeclaration(),
                                 gv->getAlignment());
  ar_gv->set_frontend(gv);
  this->_globals.try_emplace(gv, ar_gv);
  return ar_gv;
}

ar::Code* BundleImporter::translate_global_variable_initializer(
    llvm::GlobalVariable* gv) {
  ar::GlobalVariable* ar_gv = this->translate_global_variable(gv);
  ikos_assert(ar_gv->is_definition());

  // Initialize the ar::Code initializer
  ar::Code* init = ar_gv->initializer();
  ar::BasicBlock* bb = ar::BasicBlock::create(init);
  init->set_entry_block(bb);
  init->set_exit_block(bb);

  // Translate the llvm::Constant
  ar::Value* cst =
      _ctx.constant_imp->translate_constant(gv->getInitializer(),
                                            ar_gv->type()->pointee(),
                                            bb);

  // Insert statement *gv = cst
  bb->push_back(ar::Store::create(ar_gv, cst, 1, false));

  return init;
}

// This is used in the case where there is no debug information.
ar::Function* BundleImporter::translate_internal_function(llvm::Function* fun) {
  ikos_assert(!fun->isDeclaration());

  ar::Function* ar_fun = nullptr;

  // Use int for the type since it is more common than unsigned in C
  llvm::FunctionType* type = fun->getFunctionType();

  auto ar_type = ar::cast< ar::FunctionType >(
      _ctx.type_imp->translate_type(type, ar::Signed));

  ar_fun = ar::Function::create(this->_bundle,
                                ar_type,
                                fun->getName().str(),
                                /*is_definition = */ true);

  ikos_assert(ar_fun);
  return ar_fun;
}

ar::Function* BundleImporter::translate_function(llvm::Function* fun) {
  auto it = this->_functions.find(fun);

  if (it != this->_functions.end()) {
    return it->second;
  }

  // Build the ar function

  if (!fun->hasName()) {
    throw ImportError("llvm function has no name");
  }

  // Extract the DWARF type from debug information
  llvm::DISubprogram* dbg = fun->getSubprogram();

  ar::Function* ar_fun = nullptr;
  if (dbg != nullptr) {
    // Debug information available
    ar_fun = this->translate_function_di(fun, dbg);
  } else if (fun->isDeclaration()) {
    // No debug information on external function
    ar_fun = this->translate_extern_function(fun);
  } else if (this->is_clang_generated_function(fun)) {
    // Auto-generated by clang
    ar_fun = this->translate_clang_generated_function(fun);
  } else {
    // No debug information on internal function
    ar_fun = this->translate_internal_function(fun);
  }

  if (ar_fun != nullptr) {
    ar_fun->set_frontend(fun);
  }
  this->_functions.try_emplace(fun, ar_fun);
  return ar_fun;
}

ar::Function* BundleImporter::translate_function_di(llvm::Function* fun,
                                                    llvm::DISubprogram* dbg) {
  ikos_assert_msg(dbg != nullptr, "no debug info");
  ikos_assert_msg(!fun->isIntrinsic(), "unexpected intrinsic with debug info");

  // Use debug information to build the exact type
  llvm::DISubroutineType* di_type = dbg->getType();

  // Translate the function type
  ar::FunctionType* type = nullptr;

  try {
    type = _ctx.type_imp->translate_function_type(fun, di_type);
  } catch (const TypeDebugInfoMismatch&) {
    if (!this->_allow_debug_info_mismatch) {
      throw;
    }
    type = ar::cast< ar::FunctionType >(
        _ctx.type_imp->translate_type(fun->getFunctionType(), ar::Signed));
  }

  // Create the ar::Function
  return ar::Function::create(this->_bundle,
                              type,
                              fun->getName().str(),
                              /*is_definition = */ !fun->isDeclaration());
}

namespace {

/// \brief Map an LLVM floating-point type onto the matching AR float semantics.
///
/// Shared by the LLVM-intrinsic branch and the libm name mapping below so the
/// two cannot drift apart on which widths they recognise.
ar::FloatSemantic ar_float_sem_from_llvm(llvm::Type* lt) {
  if (lt->isHalfTy()) {
    return ar::Half;
  } else if (lt->isFloatTy()) {
    return ar::Float;
  } else if (lt->isDoubleTy()) {
    return ar::Double;
  } else if (lt->isX86_FP80Ty()) {
    return ar::X86_FP80;
  } else if (lt->isFP128Ty()) {
    return ar::FP128;
  } else if (lt->isPPC_FP128Ty()) {
    return ar::PPC_FP128;
  }
  return ar::Double;
}

struct LibmEntry {
  const char* name;
  ar::Intrinsic::ID id;
  unsigned arity;
};

/// math.h entry points that have an equivalent AR floating-point intrinsic.
///
/// The `f` and `l` suffix variants are listed explicitly because for a
/// non-intrinsic call the C name is all that survives into the bitcode. The
/// float width is taken from the LLVM operand type rather than from the
/// suffix, so a declaration whose suffix and type disagree cannot smuggle in
/// the wrong semantics.
///
/// Only operations the analyzer actually models appear here. `fmod`, `exp`,
/// `sin` and the rest deliberately stay opaque: inventing an image for a
/// function we have not validated is worse than saying nothing about it.
const LibmEntry kLibmTable[] = {
    {"fabs", ar::Intrinsic::FloatAbs, 1},
    {"fabsf", ar::Intrinsic::FloatAbs, 1},
    {"fabsl", ar::Intrinsic::FloatAbs, 1},

    {"sqrt", ar::Intrinsic::FloatSqrt, 1},
    {"sqrtf", ar::Intrinsic::FloatSqrt, 1},
    {"sqrtl", ar::Intrinsic::FloatSqrt, 1},

    {"log", ar::Intrinsic::FloatLog, 1},
    {"logf", ar::Intrinsic::FloatLog, 1},
    {"logl", ar::Intrinsic::FloatLog, 1},

    {"log2", ar::Intrinsic::FloatLog2, 1},
    {"log2f", ar::Intrinsic::FloatLog2, 1},
    {"log2l", ar::Intrinsic::FloatLog2, 1},

    {"log10", ar::Intrinsic::FloatLog10, 1},
    {"log10f", ar::Intrinsic::FloatLog10, 1},
    {"log10l", ar::Intrinsic::FloatLog10, 1},

    {"pow", ar::Intrinsic::FloatPow, 2},
    {"powf", ar::Intrinsic::FloatPow, 2},
    {"powl", ar::Intrinsic::FloatPow, 2},

    {"floor", ar::Intrinsic::FloatFloor, 1},
    {"floorf", ar::Intrinsic::FloatFloor, 1},
    {"floorl", ar::Intrinsic::FloatFloor, 1},

    {"ceil", ar::Intrinsic::FloatCeil, 1},
    {"ceilf", ar::Intrinsic::FloatCeil, 1},
    {"ceill", ar::Intrinsic::FloatCeil, 1},

    {"trunc", ar::Intrinsic::FloatTrunc, 1},
    {"truncf", ar::Intrinsic::FloatTrunc, 1},
    {"truncl", ar::Intrinsic::FloatTrunc, 1},

    {"round", ar::Intrinsic::FloatRound, 1},
    {"roundf", ar::Intrinsic::FloatRound, 1},
    {"roundl", ar::Intrinsic::FloatRound, 1},

    {"rint", ar::Intrinsic::FloatRint, 1},
    {"rintf", ar::Intrinsic::FloatRint, 1},
    {"rintl", ar::Intrinsic::FloatRint, 1},

    {"copysign", ar::Intrinsic::FloatCopysign, 2},
    {"copysignf", ar::Intrinsic::FloatCopysign, 2},
    {"copysignl", ar::Intrinsic::FloatCopysign, 2},

    {"fma", ar::Intrinsic::FloatFma, 3},
    {"fmaf", ar::Intrinsic::FloatFma, 3},
    {"fmal", ar::Intrinsic::FloatFma, 3},

    // C99 fmax/fmin return the numeric value of the non-NaN argument, which
    // is llvm.maxnum's absorb-NaN behaviour, not a plain comparison.
    {"fmax", ar::Intrinsic::FloatMaxnum, 2},
    {"fmaxf", ar::Intrinsic::FloatMaxnum, 2},
    {"fmaxl", ar::Intrinsic::FloatMaxnum, 2},

    {"fmin", ar::Intrinsic::FloatMinnum, 2},
    {"fminf", ar::Intrinsic::FloatMinnum, 2},
    {"fminl", ar::Intrinsic::FloatMinnum, 2},
};

} // end anonymous namespace

ar::Function* BundleImporter::translate_extern_function(llvm::Function* fun) {
  ikos_assert(fun->isDeclaration());

  ar::Function* ar_fun = nullptr;

  llvm::Intrinsic::ID id = fun->getIntrinsicID();

  // Not translated
  if (this->ignore_intrinsic(id)) {
    return nullptr;
  }

  // Translate an intrinsic function
  if (ar_fun == nullptr && fun->isIntrinsic()) {
    ar_fun = this->translate_intrinsic_function(fun, id);
  }

  // Translate known library functions (e.g, malloc, printf, etc.)
  if (ar_fun == nullptr) {
    ar_fun = this->translate_library_function(fun);
  }

  // libm entry points that arrived as plain calls rather than LLVM intrinsics.
  //
  // Whether math.h reaches the frontend as `llvm.log.f64` or as `call @log`
  // is decided by -fmath-errno, and that default is target-dependent: Apple
  // and AArch64 default to -fno-math-errno and emit the intrinsic, while
  // x86_64 Linux defaults to -fmath-errno and emits the library call. On
  // x86_64 Linux the intrinsic branch above therefore never fires, and every
  // modelled operation would silently degrade to an opaque extern -- leaving
  // the analyzer's entire floating-point domain inert on the platform most
  // users run it on. Routing the recognised names onto the same AR
  // intrinsics makes the modelling apply however the compiler spelled the
  // call. Verified with a single-variable experiment on one clang build:
  //   --target=arm64-apple-darwin            -> llvm.log.f64
  //   --target=x86_64-unknown-linux-gnu      -> @log
  //   --target=x86_64-unknown-linux-gnu -fno-math-errno -> llvm.log.f64
  if (ar_fun == nullptr) {
    ar_fun = this->translate_libm_intrinsic(fun);
  }

  if (ar_fun == nullptr) {
    // Otherwise, just prefer signed integers,
    // because int is more common than unsigned in C
    llvm::FunctionType* type = fun->getFunctionType();

    auto ar_type = ar::cast< ar::FunctionType >(
        _ctx.type_imp->translate_type(type, ar::Signed));

    ar_fun = ar::Function::create(this->_bundle,
                                  ar_type,
                                  fun->getName().str(),
                                  /*is_definition = */ false);
  }

  ikos_assert(ar_fun);
  return ar_fun;
}

bool BundleImporter::ignore_intrinsic(llvm::Intrinsic::ID id) {
  return id == llvm::Intrinsic::dbg_value ||
         id == llvm::Intrinsic::dbg_declare ||
         id == llvm::Intrinsic::dbg_label || id == llvm::Intrinsic::prefetch;
}

ar::Function* BundleImporter::translate_intrinsic_function(
    llvm::Function* fun, llvm::Intrinsic::ID id) {
  ar::Function* ar_fun = nullptr;

  if (id == llvm::Intrinsic::memcpy) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::MemoryCopy);
  } else if (id == llvm::Intrinsic::memmove) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::MemoryMove);
  } else if (id == llvm::Intrinsic::memset) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::MemorySet);
  } else if (id == llvm::Intrinsic::vastart) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::VarArgStart);
  } else if (id == llvm::Intrinsic::vaend) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::VarArgEnd);
  } else if (id == llvm::Intrinsic::vacopy) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::VarArgCopy);
  } else if (id == llvm::Intrinsic::stacksave) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::StackSave);
  } else if (id == llvm::Intrinsic::stackrestore) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::StackRestore);
  } else if (id == llvm::Intrinsic::lifetime_start) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::LifetimeStart);
  } else if (id == llvm::Intrinsic::lifetime_end) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::LifetimeEnd);
  } else if (id == llvm::Intrinsic::eh_typeid_for) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::EhTypeidFor);
  } else if (id == llvm::Intrinsic::trap) {
    ar_fun = this->_bundle->intrinsic_function(ar::Intrinsic::Trap);
  } else if (id == llvm::Intrinsic::fmuladd || id == llvm::Intrinsic::fma ||
             id == llvm::Intrinsic::fabs || id == llvm::Intrinsic::maxnum ||
             id == llvm::Intrinsic::minnum || id == llvm::Intrinsic::floor ||
             id == llvm::Intrinsic::ceil || id == llvm::Intrinsic::trunc ||
             id == llvm::Intrinsic::sqrt || id == llvm::Intrinsic::round ||
             id == llvm::Intrinsic::rint || id == llvm::Intrinsic::copysign ||
             id == llvm::Intrinsic::log || id == llvm::Intrinsic::log2 ||
             id == llvm::Intrinsic::log10 || id == llvm::Intrinsic::pow) {
    // All polymorphic: the operand type decides the AR signature, so carry it
    // through to the call site. They are NOT interchangeable -- fma is always
    // fused, fmuladd only may be, fabs is a different operation entirely,
    // maxnum/minnum absorb NaN rather than propagating it, and floor/ceil/trunc
    // round to an integer-valued float -- so each maps to its own AR intrinsic
    // and the analyzer treats them differently.
    llvm::Type* lt = fun->getArg(0)->getType();
    ar::FloatSemantic sem = ar_float_sem_from_llvm(lt);
    ar::Intrinsic::ID ar_id;
    switch (id) {
    case llvm::Intrinsic::fmuladd:
      ar_id = ar::Intrinsic::FloatFmuladd;
      break;
    case llvm::Intrinsic::fma:
      ar_id = ar::Intrinsic::FloatFma;
      break;
    case llvm::Intrinsic::fabs:
      ar_id = ar::Intrinsic::FloatAbs;
      break;
    case llvm::Intrinsic::maxnum:
      ar_id = ar::Intrinsic::FloatMaxnum;
      break;
    case llvm::Intrinsic::minnum:
      ar_id = ar::Intrinsic::FloatMinnum;
      break;
    case llvm::Intrinsic::floor:
      ar_id = ar::Intrinsic::FloatFloor;
      break;
    case llvm::Intrinsic::ceil:
      ar_id = ar::Intrinsic::FloatCeil;
      break;
    case llvm::Intrinsic::trunc:
      ar_id = ar::Intrinsic::FloatTrunc;
      break;
    case llvm::Intrinsic::sqrt:
      ar_id = ar::Intrinsic::FloatSqrt;
      break;
    case llvm::Intrinsic::round:
      ar_id = ar::Intrinsic::FloatRound;
      break;
    case llvm::Intrinsic::rint:
      ar_id = ar::Intrinsic::FloatRint;
      break;
    case llvm::Intrinsic::copysign:
      ar_id = ar::Intrinsic::FloatCopysign;
      break;
    case llvm::Intrinsic::log:
      ar_id = ar::Intrinsic::FloatLog;
      break;
    case llvm::Intrinsic::log2:
      ar_id = ar::Intrinsic::FloatLog2;
      break;
    case llvm::Intrinsic::log10:
      ar_id = ar::Intrinsic::FloatLog10;
      break;
    case llvm::Intrinsic::pow:
      ar_id = ar::Intrinsic::FloatPow;
      break;
    default:
      // Unreachable: the enclosing `else if` admits exactly the 16 intrinsics
      // cased above. This used to be `ar_id = ar::Intrinsic::FloatFmuladd`,
      // which silently misclassified anything new added to that condition as a
      // fused multiply-add -- a wrong-value bug rather than a crash. Keep the
      // switch total over the outer condition and trap on drift between them.
      ikos_unreachable("unhandled float llvm intrinsic");
    }
    ar_fun = this->_bundle->intrinsic_function(
        ar_id, ar::FloatType::get(_context, sem));
  } else {
    // No equivalent AR intrinsic, translate into a normal external function
    ar_fun = nullptr;
  }

  // Sanity check, should never happen
  // Skip for memcpy, memmove and memset because of the alignment parameter
  if (id != llvm::Intrinsic::memcpy && id != llvm::Intrinsic::memmove &&
      id != llvm::Intrinsic::memset && ar_fun != nullptr &&
      !_ctx.type_imp->match_extern_function_type(fun->getFunctionType(),
                                                 ar_fun->type())) {
    std::ostringstream buf;
    buf << "llvm intrinsic " << fun->getName().str() << " and ar intrinsic "
        << ar_fun->name() << " have a different type";
    throw ImportError(buf.str());
  }

  return ar_fun;
}

ar::Function* BundleImporter::translate_libm_intrinsic(llvm::Function* fun) {
  const LibmEntry* entry = nullptr;
  for (const LibmEntry& e : kLibmTable) {
    if (fun->getName() == e.name) {
      entry = &e;
      break;
    }
  }
  if (entry == nullptr) {
    return nullptr;
  }

  llvm::FunctionType* ft = fun->getFunctionType();
  if (ft->isVarArg() || ft->getNumParams() != entry->arity) {
    return nullptr;
  }

  // Every parameter and the return must be the very same floating-point type.
  // A function that merely shares a name with a libm entry point but does not
  // look like `T op(T...)` is not that entry point, and keeps its own
  // translation rather than being silently reinterpreted.
  llvm::Type* ret_ty = ft->getReturnType();
  if (!ret_ty->isFloatingPointTy()) {
    return nullptr;
  }
  for (llvm::Type* pt : ft->params()) {
    if (pt != ret_ty) {
      return nullptr;
    }
  }

  ar::FloatSemantic sem = ar_float_sem_from_llvm(ret_ty);
  return this->_bundle->intrinsic_function(entry->id,
                                         ar::FloatType::get(_context, sem));
}

ar::Function* BundleImporter::translate_library_function(llvm::Function* fun) {
  ar::Function* ar_fun = _ctx.lib_fun_imp->function(fun->getName());

  // Sanity check, can happen if the user uses C/C++ standard library names
  if (ar_fun != nullptr &&
      !_ctx.type_imp->match_extern_function_type(fun->getFunctionType(),
                                                 ar_fun->type())) {
    std::ostringstream buf;

    if (ar_fun->is_ikos_intrinsic()) {
      buf << "function definition of " << fun->getName().str()
          << " does not match the expected ikos intrinsic definition";
    } else if (ar_fun->is_libc_intrinsic()) {
      buf << "function definition of " << fun->getName().str()
          << " does not match the expected C Standard Library definition";
    } else if (ar_fun->is_libcpp_intrinsic()) {
      buf << "function definition of " << fun->getName().str()
          << " does not match the expected C++ Standard Library definition";
    } else {
      buf << "llvm function " << fun->getName().str() << " and ar intrinsic "
          << ar_fun->name() << " have a different type";
    }
    std::cerr << "Warning: " << buf.str() << "\n";
    std::cerr << "LLVM function declaration\n";
    std::cerr << "ikos ar expected function declaration\n";
    std::cerr << "Expected signature will be ignored.\n";
    ar_fun = nullptr;
  }

  return ar_fun;
}

bool BundleImporter::is_clang_generated_function(llvm::Function* fun) {
  if (fun->isDeclaration()) {
    return false;
  }

  if (fun->getName() == "__clang_call_terminate") {
    // Terminate function
    return true;
  }

  if (fun->getName().starts_with("_ZTW")) {
    // Thread-local wrapper function
    return true;
  }

  return false;
}

ar::Function* BundleImporter::translate_clang_generated_function(
    llvm::Function* fun) {
  llvm::FunctionType* type = fun->getFunctionType();

  // Translate the function type
  auto ar_type = ar::cast< ar::FunctionType >(
      _ctx.type_imp->translate_type(type, ar::Signed));

  return ar::Function::create(this->_bundle,
                              ar_type,
                              fun->getName().str(),
                              /*is_definition = */ !fun->isDeclaration());
}

ar::Code* BundleImporter::translate_function_body(llvm::Function* fun) {
  ar::Function* ar_fun = this->translate_function(fun);
  ikos_assert(ar_fun->is_definition());

  FunctionImporter function_imp(_ctx, fun, ar_fun);
  return function_imp.translate_body();
}

} // end namespace import
} // end namespace frontend
} // end namespace ikos

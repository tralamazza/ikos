; ModuleID = 'ikos-assert-witness.pp.bc'
source_filename = "ikos-assert-witness.c"
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.14.0"

; A witness-emitting caller (e.g. BML) declares __ikos_assert as variadic and
; appends the operands feeding the condition after argument(0). Both the
; variadic declaration and the multi-argument call must still be recognized as
; the ar.ikos.assert intrinsic. Regression guard for the import of such calls
; (see frontend/llvm/src/import/type.cpp match_extern_function_type and
; ar/src/semantic/intrinsic.cpp IkosAssert).
declare void @__ikos_assert(i32, ...) #0

define void @check(i32 %a, i32 %b) #0 {
  %cmp = icmp slt i32 %a, %b
  %cond = zext i1 %cmp to i32
  call void (i32, ...) @__ikos_assert(i32 %cond, i32 %a, i32 %b)
  ret void
}

attributes #0 = { noinline nounwind ssp uwtable }

; ---- CHECK baseline ----
; CHECK-LABEL: // Bundle
; CHECK: target-triple = x86_64-apple-macosx10.14.0
; The intrinsic type is variadic so it can carry witness operands.
; CHECK: declare void @ar.ikos.assert(ui32, ...)
; CHECK: define void @check(si32 %a, si32 %b) {
; The condition plus the two witness operands are all kept on the call.
; CHECK:   ui32 %cond = zext %cmp
; CHECK:   call @ar.ikos.assert(%cond, %a, %b)
; CHECK:   return

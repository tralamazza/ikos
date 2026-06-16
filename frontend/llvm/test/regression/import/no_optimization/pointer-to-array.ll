; Regression test for opaque-pointer recovery of pointer-to-array pointees
; (translate_di_only in import/type.cpp).
;
; A parameter `int p[][N]` is adjusted to `int (*)[N]` -- a pointer whose
; pointee is an array. Under opaque pointers the LLVM pointee is just `ptr`, so
; the array element type is recovered from debug info alone. translate_di_only
; must handle DW_TAG_array_type; otherwise the array falls through to OpaqueType
; and the parameter imports as `opaque*`, losing the inner bounds the analyzer
; needs. g must import as [4 x si32]* and h as [3 x [5 x si32]]*.

; ModuleID = 'pointer-to-array.c'
source_filename = "pointer-to-array.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.14.0"

; Function Attrs: noinline nounwind ssp uwtable
define void @g(ptr noundef %0) #0 !dbg !9 {
  %2 = alloca ptr, align 8
  store ptr %0, ptr %2, align 8
  call void @llvm.dbg.declare(metadata ptr %2, metadata !19, metadata !DIExpression()), !dbg !20
  %3 = load ptr, ptr %2, align 8, !dbg !21
  call void @use(ptr noundef %3), !dbg !22
  ret void, !dbg !23
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

declare void @use(ptr noundef) #2

; Function Attrs: noinline nounwind ssp uwtable
define void @h(ptr noundef %0) #0 !dbg !24 {
  %2 = alloca ptr, align 8
  store ptr %0, ptr %2, align 8
  call void @llvm.dbg.declare(metadata ptr %2, metadata !32, metadata !DIExpression()), !dbg !33
  %3 = load ptr, ptr %2, align 8, !dbg !34
  call void @use(ptr noundef %3), !dbg !35
  ret void, !dbg !36
}

attributes #0 = { noinline nounwind ssp uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cmov,+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #2 = { "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cmov,+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "ikos regression test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: Apple)
!1 = !DIFile(filename: "pointer-to-array.c", directory: ".")
!2 = !{i32 7, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 1, !"wchar_size", i32 4}
!5 = !{i32 8, !"PIC Level", i32 2}
!6 = !{i32 7, !"uwtable", i32 2}
!7 = !{i32 7, !"frame-pointer", i32 2}
!8 = !{!"ikos regression test"}
!9 = distinct !DISubprogram(name: "g", scope: !10, file: !10, line: 2, type: !11, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !18)
!10 = !DIFile(filename: "pointer-to-array.c", directory: ".")
!11 = !DISubroutineType(types: !12)
!12 = !{null, !13}
!13 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !14, size: 64)
!14 = !DICompositeType(tag: DW_TAG_array_type, baseType: !15, size: 128, elements: !16)
!15 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!16 = !{!17}
!17 = !DISubrange(count: 4)
!18 = !{}
!19 = !DILocalVariable(name: "p", arg: 1, scope: !9, file: !10, line: 2, type: !13)
!20 = !DILocation(line: 2, column: 12, scope: !9)
!21 = !DILocation(line: 2, column: 26, scope: !9)
!22 = !DILocation(line: 2, column: 22, scope: !9)
!23 = !DILocation(line: 2, column: 30, scope: !9)
!24 = distinct !DISubprogram(name: "h", scope: !10, file: !10, line: 3, type: !25, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !18)
!25 = !DISubroutineType(types: !26)
!26 = !{null, !27}
!27 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !28, size: 64)
!28 = !DICompositeType(tag: DW_TAG_array_type, baseType: !15, size: 480, elements: !29)
!29 = !{!30, !31}
!30 = !DISubrange(count: 3)
!31 = !DISubrange(count: 5)
!32 = !DILocalVariable(name: "p", arg: 1, scope: !24, file: !10, line: 3, type: !27)
!33 = !DILocation(line: 3, column: 12, scope: !24)
!34 = !DILocation(line: 3, column: 29, scope: !24)
!35 = !DILocation(line: 3, column: 25, scope: !24)
!36 = !DILocation(line: 3, column: 33, scope: !24)

; ---- CHECK baseline ----
; The pointer-to-array parameters must recover the array pointee from DI; before
; translate_di_only handled DW_TAG_array_type they imported as `opaque*`.
;
; CHECK-LABEL: // Bundle
; CHECK: define void @g([4 x si32]* %1) {
; CHECK: define void @h([3 x [5 x si32]]* %1) {

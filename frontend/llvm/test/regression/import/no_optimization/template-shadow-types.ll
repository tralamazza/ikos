; Regression test for opaque-pointer struct recovery (translate_di_only /
; candidate_structs_by_di in import/type.cpp).
;
; Box<int> and Box<float> are two same-base-name, same-size (64-bit)
; instantiations. Clang names them %struct.Box and %struct.Box.0; debug info
; calls both "Box<...>", which strips to the base name "Box". Under opaque
; pointers the importer recovers each pointer parameter's pointee from debug
; info alone. Matching the DI by name + size only would resolve both to the
; first same-sized candidate (%struct.Box = {i32, i32}); for use_f that mismatch
; would throw and fall back to si8*, dropping the field layout. The importer must
; disambiguate with the full DI reconciliation so use_f's pointee is recovered
; as {float, float}, not {i32, i32} or si8*.

; ModuleID = 'template-shadow-types.cpp'
source_filename = "template-shadow-types.cpp"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.14.0"

%struct.Box = type { i32, i32 }
%struct.Box.0 = type { float, float }

@__const.main.bi = private unnamed_addr constant %struct.Box { i32 1, i32 2 }, align 4
@__const.main.bf = private unnamed_addr constant %struct.Box.0 { float 1.000000e+00, float 2.000000e+00 }, align 4

; Function Attrs: mustprogress noinline nounwind ssp uwtable
define noundef i32 @_Z5use_iP3BoxIiE(ptr noundef %0) #0 !dbg !11 {
  %2 = alloca ptr, align 8
  store ptr %0, ptr %2, align 8
  call void @llvm.dbg.declare(metadata ptr %2, metadata !23, metadata !DIExpression()), !dbg !24
  %3 = load ptr, ptr %2, align 8, !dbg !25
  %4 = getelementptr inbounds %struct.Box, ptr %3, i32 0, i32 0, !dbg !26
  %5 = load i32, ptr %4, align 4, !dbg !26
  %6 = load ptr, ptr %2, align 8, !dbg !27
  %7 = getelementptr inbounds %struct.Box, ptr %6, i32 0, i32 1, !dbg !28
  %8 = load i32, ptr %7, align 4, !dbg !28
  %9 = add nsw i32 %5, %8, !dbg !29
  ret i32 %9, !dbg !30
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: mustprogress noinline nounwind ssp uwtable
define noundef float @_Z5use_fP3BoxIfE(ptr noundef %0) #0 !dbg !31 {
  %2 = alloca ptr, align 8
  store ptr %0, ptr %2, align 8
  call void @llvm.dbg.declare(metadata ptr %2, metadata !42, metadata !DIExpression()), !dbg !43
  %3 = load ptr, ptr %2, align 8, !dbg !44
  %4 = getelementptr inbounds %struct.Box.0, ptr %3, i32 0, i32 0, !dbg !45
  %5 = load float, ptr %4, align 4, !dbg !45
  %6 = load ptr, ptr %2, align 8, !dbg !46
  %7 = getelementptr inbounds %struct.Box.0, ptr %6, i32 0, i32 1, !dbg !47
  %8 = load float, ptr %7, align 4, !dbg !47
  %9 = fadd float %5, %8, !dbg !48
  ret float %9, !dbg !49
}

; Function Attrs: mustprogress noinline norecurse nounwind ssp uwtable
define noundef i32 @main() #2 !dbg !50 {
  %1 = alloca i32, align 4
  %2 = alloca %struct.Box, align 4
  %3 = alloca %struct.Box.0, align 4
  store i32 0, ptr %1, align 4
  call void @llvm.dbg.declare(metadata ptr %2, metadata !52, metadata !DIExpression()), !dbg !53
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 %2, ptr align 4 @__const.main.bi, i64 8, i1 false), !dbg !53
  call void @llvm.dbg.declare(metadata ptr %3, metadata !54, metadata !DIExpression()), !dbg !55
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 %3, ptr align 4 @__const.main.bf, i64 8, i1 false), !dbg !55
  %4 = call noundef i32 @_Z5use_iP3BoxIiE(ptr noundef %2), !dbg !56
  %5 = call noundef float @_Z5use_fP3BoxIfE(ptr noundef %3), !dbg !57
  %6 = fptosi float %5 to i32, !dbg !57
  %7 = add nsw i32 %4, %6, !dbg !58
  ret i32 %7, !dbg !59
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg) #3

attributes #0 = { mustprogress noinline nounwind ssp uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cmov,+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #2 = { mustprogress noinline norecurse nounwind ssp uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cmov,+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }
attributes #3 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!4, !5, !6, !7, !8, !9}
!llvm.ident = !{!10}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, producer: "ikos regression test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !2, splitDebugInlining: false, nameTableKind: Apple)
!1 = !DIFile(filename: "template-shadow-types.cpp", directory: ".")
!2 = !{!3}
!3 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!4 = !{i32 7, !"Dwarf Version", i32 4}
!5 = !{i32 2, !"Debug Info Version", i32 3}
!6 = !{i32 1, !"wchar_size", i32 4}
!7 = !{i32 8, !"PIC Level", i32 2}
!8 = !{i32 7, !"uwtable", i32 2}
!9 = !{i32 7, !"frame-pointer", i32 2}
!10 = !{!"ikos regression test"}
!11 = distinct !DISubprogram(name: "use_i", linkageName: "_Z5use_iP3BoxIiE", scope: !12, file: !12, line: 3, type: !13, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !22)
!12 = !DIFile(filename: "template-shadow-types.cpp", directory: ".")
!13 = !DISubroutineType(types: !14)
!14 = !{!3, !15}
!15 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !16, size: 64)
!16 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Box<int>", file: !12, line: 1, size: 64, flags: DIFlagTypePassByValue, elements: !17, templateParams: !20, identifier: "_ZTS3BoxIiE")
!17 = !{!18, !19}
!18 = !DIDerivedType(tag: DW_TAG_member, name: "a", scope: !16, file: !12, line: 1, baseType: !3, size: 32)
!19 = !DIDerivedType(tag: DW_TAG_member, name: "b", scope: !16, file: !12, line: 1, baseType: !3, size: 32, offset: 32)
!20 = !{!21}
!21 = !DITemplateTypeParameter(name: "T", type: !3)
!22 = !{}
!23 = !DILocalVariable(name: "p", arg: 1, scope: !11, file: !12, line: 3, type: !15)
!24 = !DILocation(line: 3, column: 49, scope: !11)
!25 = !DILocation(line: 3, column: 61, scope: !11)
!26 = !DILocation(line: 3, column: 64, scope: !11)
!27 = !DILocation(line: 3, column: 68, scope: !11)
!28 = !DILocation(line: 3, column: 71, scope: !11)
!29 = !DILocation(line: 3, column: 66, scope: !11)
!30 = !DILocation(line: 3, column: 54, scope: !11)
!31 = distinct !DISubprogram(name: "use_f", linkageName: "_Z5use_fP3BoxIfE", scope: !12, file: !12, line: 4, type: !32, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !22)
!32 = !DISubroutineType(types: !33)
!33 = !{!34, !35}
!34 = !DIBasicType(name: "float", size: 32, encoding: DW_ATE_float)
!35 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !36, size: 64)
!36 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Box<float>", file: !12, line: 1, size: 64, flags: DIFlagTypePassByValue, elements: !37, templateParams: !40, identifier: "_ZTS3BoxIfE")
!37 = !{!38, !39}
!38 = !DIDerivedType(tag: DW_TAG_member, name: "a", scope: !36, file: !12, line: 1, baseType: !34, size: 32)
!39 = !DIDerivedType(tag: DW_TAG_member, name: "b", scope: !36, file: !12, line: 1, baseType: !34, size: 32, offset: 32)
!40 = !{!41}
!41 = !DITemplateTypeParameter(name: "T", type: !34)
!42 = !DILocalVariable(name: "p", arg: 1, scope: !31, file: !12, line: 4, type: !35)
!43 = !DILocation(line: 4, column: 51, scope: !31)
!44 = !DILocation(line: 4, column: 63, scope: !31)
!45 = !DILocation(line: 4, column: 66, scope: !31)
!46 = !DILocation(line: 4, column: 70, scope: !31)
!47 = !DILocation(line: 4, column: 73, scope: !31)
!48 = !DILocation(line: 4, column: 68, scope: !31)
!49 = !DILocation(line: 4, column: 56, scope: !31)
!50 = distinct !DISubprogram(name: "main", scope: !12, file: !12, line: 5, type: !51, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !22)
!51 = !DISubroutineType(types: !2)
!52 = !DILocalVariable(name: "bi", scope: !50, file: !12, line: 6, type: !16)
!53 = !DILocation(line: 6, column: 14, scope: !50)
!54 = !DILocalVariable(name: "bf", scope: !50, file: !12, line: 7, type: !36)
!55 = !DILocation(line: 7, column: 14, scope: !50)
!56 = !DILocation(line: 8, column: 10, scope: !50)
!57 = !DILocation(line: 8, column: 28, scope: !50)
!58 = !DILocation(line: 8, column: 21, scope: !50)
!59 = !DILocation(line: 8, column: 3, scope: !50)

; ---- CHECK baseline ----
; use_f's pointee must be {float, float}, recovered via DI despite %struct.Box
; (the int instantiation) being the first same-sized name match.
;
; CHECK-LABEL: // Bundle
; CHECK: define float @_Z5use_fP3BoxIfE({0: float, 4: float}* %1) {
; CHECK: define si32 @_Z5use_iP3BoxIiE({0: si32, 4: si32}* %1) {

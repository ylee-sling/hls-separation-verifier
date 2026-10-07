; Acceptance row unresolved: the pointer is a select of two arrays, so the
; access cannot be mapped to Gamma. It is serialised, and it taints the other
; group in its region. Parallel twin: the same A loads without the select
; (@resolved).
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @merged
; CHECK:       @A: Gamma
; CHECK:       status: Unknown (paper: Unknown) -> Sequential
; CHECK-NEXT:  reason: region also contains an unresolved memory access (pointer merges several objects (phi/select))
; CHECK:       <unresolved base>
; CHECK:       %v = load i32, ptr %p
; CHECK:       status: Unknown (paper: Unknown) -> Sequential
; CHECK-NEXT:  reason: cannot map to Gamma: pointer merges several objects (phi/select)

; CHECK-LABEL: HLS spatial verification: @resolved
; CHECK:       status: Proven (paper: Sat) -> Parallel
; CHECK-NOT:   unresolved

@A = global [64 x i32] zeroinitializer, !hls.partition !0
@B = global [64 x i32] zeroinitializer, !hls.partition !0

define i32 @merged(i1 %c) {
entry:
  %a0 = load i32, ptr @A, align 4
  %q = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 1
  %a1 = load i32, ptr %q, align 4
  %p = select i1 %c, ptr @A, ptr @B
  %v = load i32, ptr %p, align 4
  %s1 = add i32 %a0, %a1
  %s2 = add i32 %s1, %v
  ret i32 %s2
}

define i32 @resolved(i1 %c) {
entry:
  %a0 = load i32, ptr @A, align 4
  %q = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 1
  %a1 = load i32, ptr %q, align 4
  %s1 = add i32 %a0, %a1
  ret i32 %s1
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 4}

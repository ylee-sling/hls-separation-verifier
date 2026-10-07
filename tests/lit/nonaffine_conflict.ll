; Acceptance row nonaffine_conflict: A[x*y] and A[x*y+4], cyclic N=4. The
; non-affine product is carried to the oracle as-is; (m+4) mod 4 = m mod 4,
; so the two claims always share a bank. Parallel twin: A[x*y] and
; A[x*y+1] (@shift1), proven distinct under QF_BV wrap-around.
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @prod4
; CHECK:       @A: Gamma = (alpha=0, M=64, cyclic), N=4, elem=4B
; CHECK:       over 2 claim(s)
; CHECK:       status: Refuted (paper: Unsat) -> Sequential
; CHECK-NEXT:  reason: counterexample for claims #0 and #1: both claim bank

; CHECK-LABEL: HLS spatial verification: @shift1
; CHECK:       status: Proven (paper: Sat) -> Parallel

@A = global [64 x i32] zeroinitializer, !hls.partition !0

define i32 @prod4(i64 %x, i64 %y) {
entry:
  %m = mul i64 %x, %y
  %p0 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %m
  %v0 = load i32, ptr %p0, align 4
  %m4 = add i64 %m, 4
  %p1 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %m4
  %v1 = load i32, ptr %p1, align 4
  %s = add i32 %v0, %v1
  ret i32 %s
}

define i32 @shift1(i64 %x, i64 %y) {
entry:
  %m = mul i64 %x, %y
  %p0 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %m
  %v0 = load i32, ptr %p0, align 4
  %m1 = add i64 %m, 1
  %p1 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %m1
  %v1 = load i32, ptr %p1, align 4
  %s = add i32 %v0, %v1
  ret i32 %s
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 4}

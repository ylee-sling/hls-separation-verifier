; Acceptance row complete_ok: M=8 complete partitioning (N = M); eight loads
; at constant offsets 0..7 at function top level. Every k_i is a distinct
; numeral, so all pairs are separated without a solver query.
; Sequential twin: the same array read twice at offset 3 (@twice).
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @sum8
; CHECK:       region: function top level
; CHECK:       @A: Gamma = (alpha=0, M=8, complete), N=8, elem=4B
; CHECK:       over 8 claim(s)
; CHECK:       status: Proven (paper: Sat) -> Parallel
; CHECK-NEXT:  reason: all claim pairs separated syntactically

; CHECK-LABEL: HLS spatial verification: @twice
; CHECK:       status: Refuted (paper: Unsat) -> Sequential
; CHECK-NEXT:  reason: spatial collapse between claims #0 and #1

@A = global [8 x i32] zeroinitializer, !hls.partition !0

define i32 @sum8() {
entry:
  %v0 = load i32, ptr @A, align 4
  %p1 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 1
  %v1 = load i32, ptr %p1, align 4
  %p2 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 2
  %v2 = load i32, ptr %p2, align 4
  %p3 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 3
  %v3 = load i32, ptr %p3, align 4
  %p4 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 4
  %v4 = load i32, ptr %p4, align 4
  %p5 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 5
  %v5 = load i32, ptr %p5, align 4
  %p6 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 6
  %v6 = load i32, ptr %p6, align 4
  %p7 = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 7
  %v7 = load i32, ptr %p7, align 4
  %s1 = add i32 %v0, %v1
  %s2 = add i32 %s1, %v2
  %s3 = add i32 %s2, %v3
  %s4 = add i32 %s3, %v4
  %s5 = add i32 %s4, %v5
  %s6 = add i32 %s5, %v6
  %s7 = add i32 %s6, %v7
  ret i32 %s7
}

define i32 @twice() {
entry:
  %p = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 3
  %a = load i32, ptr %p, align 4
  %q = getelementptr inbounds [8 x i32], ptr @A, i64 0, i64 3
  %b = load i32, ptr %q, align 4
  %s = add i32 %a, %b
  ret i32 %s
}

!0 = !{!"scheme", !"complete"}

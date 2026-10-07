; Acceptance row rmw_collapse: A[i] = A[i] * 3. The load and the store claim
; the same element, hence the same single-port bank: strict exclusive
; ownership collapses (paper 4.2; fractional permissions are future work).
; This is the intended verdict, not a false positive (CLAUDE.md invariant 11).
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @triple
; CHECK:       region: loop %loop
; CHECK:       over 2 claim(s)
; CHECK:       load i32, ptr %p
; CHECK:       store i32 %t, ptr %p
; CHECK:       status: Refuted (paper: Unsat) -> Sequential
; CHECK:       (0,1) Refuted

@A = global [64 x i32] zeroinitializer, !hls.partition !0

define void @triple() {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v = load i32, ptr %p, align 4
  %t = mul i32 %v, 3
  store i32 %t, ptr %p, align 4
  %i.next = add nuw nsw i64 %i, 1
  %cmp = icmp ult i64 %i.next, 64
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 4}

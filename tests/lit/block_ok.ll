; Acceptance row block_ok: M=64 block N=4 (B=16); loads A[i], A[i+16],
; A[i+32], A[i+48] for i < 16. Banks 0..3. Sequential twin: block_conflict.ll.
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @sum
; CHECK:       region: loop %loop
; CHECK:       @A: Gamma = (alpha=0, M=64, block), N=4, elem=4B, B=16
; CHECK:       over 4 claim(s)
; CHECK:       status: Proven (paper: Sat) -> Parallel

@A = global [64 x i32] zeroinitializer, !hls.partition !0

define i32 @sum() {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.4, %loop ]
  %p0 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v0 = load i32, ptr %p0, align 4
  %i1 = add nuw nsw i64 %i, 16
  %p1 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i1
  %v1 = load i32, ptr %p1, align 4
  %i2 = add nuw nsw i64 %i, 32
  %p2 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i2
  %v2 = load i32, ptr %p2, align 4
  %i3 = add nuw nsw i64 %i, 48
  %p3 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i3
  %v3 = load i32, ptr %p3, align 4
  %acc.1 = add i32 %acc, %v0
  %acc.2 = add i32 %acc.1, %v1
  %acc.3 = add i32 %acc.2, %v2
  %acc.4 = add i32 %acc.3, %v3
  %i.next = add nuw nsw i64 %i, 1
  %cmp = icmp ult i64 %i.next, 16
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.4
}

!0 = !{!"scheme", !"block", !"factor", i32 4}

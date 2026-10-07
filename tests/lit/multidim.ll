; Acceptance row multidim: @B = [8 x [8 x i32]] cyclic N=8; the row loop over
; x is fully unrolled over the columns, loading B[x][0..7]. The flat offset
; 8*x + c comes from the GEP (collectOffset), not from a C-shape rewrite, so
; the bank of column c is c. Sequential twin: N=4 via the command line,
; where columns c and c+4 collide.
; RUN: %hls-sv %s 2>&1 | FileCheck %s --check-prefix=N8
; RUN: %hls-sv -hls-sv-partition=B:cyclic:4 %s 2>&1 | FileCheck %s --check-prefix=N4

; N8-LABEL: HLS spatial verification: @rowsum
; N8:       region: loop %loop
; N8:       @B: Gamma = (alpha=0, M=64, cyclic), N=8, elem=4B
; N8:       over 8 claim(s)
; N8:       expr = (bvmul #x0000000000000008 %x)
; N8:       status: Proven (paper: Sat) -> Parallel

; N4-LABEL: HLS spatial verification: @rowsum
; N4:       @B: Gamma = (alpha=0, M=64, cyclic), N=4, elem=4B
; N4:       status: Refuted (paper: Unsat) -> Sequential
; N4:       (0,4) Refuted

@B = global [8 x [8 x i32]] zeroinitializer, !hls.partition !0

define i32 @rowsum() {
entry:
  br label %loop

loop:
  %x = phi i64 [ 0, %entry ], [ %x.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.8, %loop ]
  %p0 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 0
  %v0 = load i32, ptr %p0, align 4
  %p1 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 1
  %v1 = load i32, ptr %p1, align 4
  %p2 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 2
  %v2 = load i32, ptr %p2, align 4
  %p3 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 3
  %v3 = load i32, ptr %p3, align 4
  %p4 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 4
  %v4 = load i32, ptr %p4, align 4
  %p5 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 5
  %v5 = load i32, ptr %p5, align 4
  %p6 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 6
  %v6 = load i32, ptr %p6, align 4
  %p7 = getelementptr inbounds [8 x [8 x i32]], ptr @B, i64 0, i64 %x, i64 7
  %v7 = load i32, ptr %p7, align 4
  %acc.1 = add i32 %acc, %v0
  %acc.2 = add i32 %acc.1, %v1
  %acc.3 = add i32 %acc.2, %v2
  %acc.4 = add i32 %acc.3, %v3
  %acc.5 = add i32 %acc.4, %v4
  %acc.6 = add i32 %acc.5, %v5
  %acc.7 = add i32 %acc.6, %v6
  %acc.8 = add i32 %acc.7, %v7
  %x.next = add nuw nsw i64 %x, 1
  %cmp = icmp ult i64 %x.next, 8
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.8
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 8}

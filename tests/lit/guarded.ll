; Acceptance row guarded: A[i] and A[i+4] (same bank under cyclic N=4) in
; mutually exclusive if/else arms. The claim guards c and !c contradict, so
; the pair cannot co-execute. Sequential twin: the same two loads without the
; branch (@unguarded).
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @pick
; CHECK:       region: function top level
; CHECK:       over 2 claim(s)
; CHECK:       status: Proven (paper: Sat) -> Parallel

; CHECK-LABEL: HLS spatial verification: @unguarded
; CHECK:       status: Refuted (paper: Unsat) -> Sequential

@A = global [64 x i32] zeroinitializer, !hls.partition !0

define i32 @pick(i1 %c, i64 %i) {
entry:
  br i1 %c, label %then, label %else

then:
  %p0 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v0 = load i32, ptr %p0, align 4
  br label %join

else:
  %i4 = add i64 %i, 4
  %p1 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i4
  %v1 = load i32, ptr %p1, align 4
  br label %join

join:
  %r = phi i32 [ %v0, %then ], [ %v1, %else ]
  ret i32 %r
}

define i32 @unguarded(i1 %c, i64 %i) {
entry:
  %p0 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v0 = load i32, ptr %p0, align 4
  %i4 = add i64 %i, 4
  %p1 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i4
  %v1 = load i32, ptr %p1, align 4
  %r = select i1 %c, i32 %v0, i32 %v1
  ret i32 %r
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 4}

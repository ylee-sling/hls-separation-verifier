; Acceptance row undecidable_fallback: non-linear index pair
; (x*y) urem 7 vs (x*y+1) urem 7 into @A[7] cyclic N=7. The pair is not
; decided syntactically, and a tiny deterministic rlimit (T_out) makes Z3
; give up; M(Tout) = Sequential. With the default budget the same query is
; proven, which shows the RUN 1 verdict is the fallback and not a conflict.
; RUN: %hls-sv -hls-sv-rlimit=1 %s 2>&1 | FileCheck %s --check-prefix=TOUT
; RUN: %hls-sv %s 2>&1 | FileCheck %s --check-prefix=FULL

; TOUT-LABEL: HLS spatial verification: @mod7
; TOUT:       @A: Gamma = (alpha=0, M=7, cyclic), N=7, elem=4B
; TOUT:       over 2 claim(s)
; TOUT:       status: Timeout (paper: Tout) -> Sequential
; TOUT-NEXT:  reason: solver inconclusive:
; TOUT-NOT:   separated syntactically
; TOUT:       (0,1) Timeout:

; FULL-LABEL: HLS spatial verification: @mod7
; FULL:       status: Proven (paper: Sat) -> Parallel

@A = global [7 x i32] zeroinitializer, !hls.partition !0

define i32 @mod7(i64 %x, i64 %y) {
entry:
  %m = mul i64 %x, %y
  %r0 = urem i64 %m, 7
  %p0 = getelementptr inbounds [7 x i32], ptr @A, i64 0, i64 %r0
  %v0 = load i32, ptr %p0, align 4
  %m1 = add i64 %m, 1
  %r1 = urem i64 %m1, 7
  %p1 = getelementptr inbounds [7 x i32], ptr @A, i64 0, i64 %r1
  %v1 = load i32, ptr %p1, align 4
  %s = add i32 %v0, %v1
  ret i32 %s
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 7}

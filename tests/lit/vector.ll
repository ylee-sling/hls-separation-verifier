; Acceptance row vector: one <4 x i32> load at A[i]. Each lane is a claim:
; offsets i..i+3 are pairwise distinct mod 4 (Parallel) but lanes 0/2 and
; 1/3 share a bank mod 2 (Sequential).
; RUN: %hls-sv %s 2>&1 | FileCheck %s --check-prefix=N4
; RUN: %hls-sv -hls-sv-partition=A:cyclic:2 %s 2>&1 | FileCheck %s --check-prefix=N2

; N4-LABEL: HLS spatial verification: @vload
; N4:       N=4, elem=4B
; N4:       over 4 claim(s)
; N4:       (lane 3)
; N4:       status: Proven (paper: Sat) -> Parallel

; N2-LABEL: HLS spatial verification: @vload
; N2:       N=2, elem=4B
; N2:       over 4 claim(s)
; N2:       status: Refuted (paper: Unsat) -> Sequential
; N2:       (0,2) Refuted
; N2:       (1,3) Refuted

@A = global [64 x i32] zeroinitializer, !hls.partition !0

define <4 x i32> @vload(i64 %i) {
entry:
  %p = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v = load <4 x i32>, ptr %p, align 16
  ret <4 x i32> %v
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 4}

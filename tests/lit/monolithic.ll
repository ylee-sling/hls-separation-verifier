; Acceptance row monolithic: @A has no partition directive, so it is one
; single-port SRAM (N = 1): every bank index is 0 and two accesses in one
; region always collapse. Parallel twin: the same loads with cyclic N=2
; given on the command line.
; RUN: %hls-sv %s 2>&1 | FileCheck %s --check-prefix=MONO
; RUN: %hls-sv -hls-sv-partition=A:cyclic:2 %s 2>&1 | FileCheck %s --check-prefix=PART

; MONO-LABEL: HLS spatial verification: @two
; MONO:       @A: Gamma = (alpha=0, M=64, cyclic), N=1, elem=4B [implicit monolithic SRAM]
; MONO:       over 2 claim(s)
; MONO:       k = #x0000000000000000
; MONO:       k = #x0000000000000000
; MONO:       status: Refuted (paper: Unsat) -> Sequential
; MONO-NEXT:  reason: spatial collapse between claims #0 and #1

; PART-LABEL: HLS spatial verification: @two
; PART:       N=2, elem=4B
; PART-NOT:   implicit
; PART:       status: Proven (paper: Sat) -> Parallel

@A = global [64 x i32] zeroinitializer

define i32 @two() {
entry:
  %a0 = load i32, ptr @A, align 4
  %p = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 1
  %a1 = load i32, ptr %p, align 4
  %s = add i32 %a0, %a1
  ret i32 %s
}

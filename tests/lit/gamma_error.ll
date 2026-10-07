; Acceptance row gamma_error: block partitioning on a `ptr` argument without
; a capacity. M cannot be recovered from an opaque pointer, so Gamma cannot be
; built; every access takes the conservative Sequential fallback.
; Parallel twin: the same directive with "capacity" (@with_capacity).
; RUN: %hls-sv %s 2>&1 | FileCheck %s

; CHECK-LABEL: HLS spatial verification: @no_capacity
; CHECK-NEXT:  environment error: Gamma: block partitioning of @A needs a known capacity M
; CHECK-NEXT:  -> every memory access Sequential (conservative fallback)

; CHECK-LABEL: HLS spatial verification: @with_capacity
; CHECK:       @A: Gamma = ({{.*}}, M=64, block), N=4, elem=4B, B=16
; CHECK:       status: Proven (paper: Sat) -> Parallel

define i32 @no_capacity(ptr %A) !hls.partition.args !0 {
entry:
  %a0 = load i32, ptr %A, align 4
  %p = getelementptr inbounds i32, ptr %A, i64 16
  %a1 = load i32, ptr %p, align 4
  %s = add i32 %a0, %a1
  ret i32 %s
}

define i32 @with_capacity(ptr %A) !hls.partition.args !2 {
entry:
  %a0 = load i32, ptr %A, align 4
  %p = getelementptr inbounds i32, ptr %A, i64 16
  %a1 = load i32, ptr %p, align 4
  %s = add i32 %a0, %a1
  ret i32 %s
}

!0 = !{!1}
!1 = !{!"arg", i32 0, !"scheme", !"block", !"factor", i32 4, !"elem_bytes", i64 4}
!2 = !{!3}
!3 = !{!"arg", i32 0, !"scheme", !"block", !"factor", i32 4, !"capacity", i64 64, !"elem_bytes", i64 4}

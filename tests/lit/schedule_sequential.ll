; hls-schedule-annotate, sequential mode:
;   !{!"sequential", i32 <group>, i32 <issue_slot>, !"<status>"}
; issue_slot is the program-order position within the group. One remark per
; Sequential group carries the reason.
;  - @conflict: cyclic N=2, four loads -> Refuted, slots 0..3.
;  - @rmw:      load + store of A[i] -> Refuted, slots 0 (load), 1 (store).
;  - @vload:    one <4 x i32> load, N=2 -> Refuted, one directive, slot 0.
;  - @merged:   unresolved select pointer -> two Unknown groups (0 and 1).
;  - @no_gamma: environment error -> no directive at all, one remark.
;  - @stale:    pre-existing !hls.schedule is replaced (load) or dropped
;               (non-memory instruction); a second run gives the same result.
; Timeout is checked on undecidable_fallback.ll with a tiny rlimit.
;
; RUN: opt -load-pass-plugin %plugin -passes=hls-schedule-annotate -S %s | FileCheck %s
; RUN: opt -load-pass-plugin %plugin -passes=hls-schedule-annotate,hls-schedule-annotate -S %s \
; RUN:   | FileCheck %s
; RUN: opt -load-pass-plugin %plugin -passes=hls-schedule-annotate \
; RUN:     -pass-remarks-analysis=hls-spatial-verify -disable-output %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=REMARK
; RUN: opt -load-pass-plugin %plugin -passes=hls-schedule-annotate -hls-sv-rlimit=1 \
; RUN:     -S %S/undecidable_fallback.ll | FileCheck %s --check-prefix=TOUT

; CHECK-LABEL: define i32 @conflict()
; CHECK:       %v0 = load i32, ptr %p0, align 4, !hls.schedule ![[C0:[0-9]+]]
; CHECK:       %v1 = load i32, ptr %p1, align 4, !hls.schedule ![[C1:[0-9]+]]
; CHECK:       %v2 = load i32, ptr %p2, align 4, !hls.schedule ![[C2:[0-9]+]]
; CHECK:       %v3 = load i32, ptr %p3, align 4, !hls.schedule ![[C3:[0-9]+]]
; CHECK:       %acc.1 = add i32 %acc, %v0{{$}}

; CHECK-LABEL: define void @rmw()
; CHECK:       %v = load i32, ptr %p, align 4, !hls.schedule ![[C0]]
; CHECK:       %t = mul i32 %v, 3{{$}}
; CHECK:       store i32 %t, ptr %p, align 4, !hls.schedule ![[C1]]

; CHECK-LABEL: define <4 x i32> @vload(i64 %i)
; CHECK:       load <4 x i32>, ptr %p, align 16, !hls.schedule ![[C0]]

; CHECK-LABEL: define i32 @merged(i1 %c)
; CHECK:       %a0 = load i32, ptr @A, align 4, !hls.schedule ![[U00:[0-9]+]]
; CHECK:       %a1 = load i32, ptr %q, align 4, !hls.schedule ![[U01:[0-9]+]]
; CHECK:       %v = load i32, ptr %p, align 4, !hls.schedule ![[U10:[0-9]+]]

; CHECK-LABEL: define i32 @no_gamma(ptr %B)
; CHECK:       %b0 = load i32, ptr %B, align 4{{$}}
; CHECK:       %b1 = load i32, ptr %q, align 4{{$}}

; CHECK-LABEL: define i32 @stale()
; CHECK:       %a0 = load i32, ptr @A, align 4, !hls.schedule ![[C0]]
; CHECK:       %a1 = load i32, ptr %q, align 4, !hls.schedule ![[C1]]
; CHECK:       %s = add i32 %a0, %a1{{$}}

; CHECK-DAG:   ![[C0]] = !{!"sequential", i32 0, i32 0, !"Refuted"}
; CHECK-DAG:   ![[C1]] = !{!"sequential", i32 0, i32 1, !"Refuted"}
; CHECK-DAG:   ![[C2]] = !{!"sequential", i32 0, i32 2, !"Refuted"}
; CHECK-DAG:   ![[C3]] = !{!"sequential", i32 0, i32 3, !"Refuted"}
; CHECK-DAG:   ![[U00]] = !{!"sequential", i32 0, i32 0, !"Unknown"}
; CHECK-DAG:   ![[U01]] = !{!"sequential", i32 0, i32 1, !"Unknown"}
; CHECK-DAG:   ![[U10]] = !{!"sequential", i32 1, i32 0, !"Unknown"}
; CHECK-NOT:   !"parallel"

; REMARK-DAG:  remark: {{.*}}group 0 (@A: Gamma = (alpha=0, M=64, cyclic), N=2, elem=4B): Refuted -> Sequential, 4 access(es) serialised: counterexample for claims
; REMARK-DAG:  remark: {{.*}}group 0 ({{.*}}): Refuted -> Sequential, 2 access(es) serialised:
; REMARK-DAG:  remark: {{.*}}group 0 ({{.*}}): Refuted -> Sequential, 1 access(es) serialised:
; REMARK-DAG:  remark: {{.*}}group 0 ({{.*}}): Unknown -> Sequential, 2 access(es) serialised: region also contains an unresolved memory access
; REMARK-DAG:  remark: {{.*}}group 1 (<unresolved base>): Unknown -> Sequential, 1 access(es) serialised: cannot map to Gamma: pointer merges several objects (phi/select)
; REMARK-DAG:  remark: {{.*}}environment error: Gamma: block partitioning of @B needs a known capacity M; every memory access Sequential

; TOUT-LABEL: define i32 @mod7(
; TOUT:       %v0 = load i32, ptr %p0, align 4, !hls.schedule ![[T0:[0-9]+]]
; TOUT:       %v1 = load i32, ptr %p1, align 4, !hls.schedule ![[T1:[0-9]+]]
; TOUT-DAG:   ![[T0]] = !{!"sequential", i32 0, i32 0, !"Timeout"}
; TOUT-DAG:   ![[T1]] = !{!"sequential", i32 0, i32 1, !"Timeout"}

@A = global [64 x i32] zeroinitializer, !hls.partition !0
@C = global [64 x i32] zeroinitializer

define i32 @conflict() {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.4, %loop ]
  %p0 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v0 = load i32, ptr %p0, align 4
  %i1 = or disjoint i64 %i, 1
  %p1 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i1
  %v1 = load i32, ptr %p1, align 4
  %i2 = or disjoint i64 %i, 2
  %p2 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i2
  %v2 = load i32, ptr %p2, align 4
  %i3 = or disjoint i64 %i, 3
  %p3 = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i3
  %v3 = load i32, ptr %p3, align 4
  %acc.1 = add i32 %acc, %v0
  %acc.2 = add i32 %acc.1, %v1
  %acc.3 = add i32 %acc.2, %v2
  %acc.4 = add i32 %acc.3, %v3
  %i.next = add nuw nsw i64 %i, 4
  %cmp = icmp ult i64 %i.next, 64
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.4
}

define void @rmw() {
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

define <4 x i32> @vload(i64 %i) {
entry:
  %p = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v = load <4 x i32>, ptr %p, align 16
  ret <4 x i32> %v
}

define i32 @merged(i1 %c) {
entry:
  %a0 = load i32, ptr @A, align 4
  %q = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 1
  %a1 = load i32, ptr %q, align 4
  %p = select i1 %c, ptr @A, ptr @C
  %v = load i32, ptr %p, align 4
  %s1 = add i32 %a0, %a1
  %s2 = add i32 %s1, %v
  ret i32 %s2
}

define i32 @no_gamma(ptr %B) !hls.partition.args !1 {
entry:
  %b0 = load i32, ptr %B, align 4
  %q = getelementptr inbounds i32, ptr %B, i64 16
  %b1 = load i32, ptr %q, align 4
  %s = add i32 %b0, %b1
  ret i32 %s
}

; @A[0] and @A[2] share a bank under N=2; the stale "parallel" must not survive.
define i32 @stale() {
entry:
  %a0 = load i32, ptr @A, align 4, !hls.schedule !3
  %q = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 2
  %a1 = load i32, ptr %q, align 4, !hls.schedule !3
  %s = add i32 %a0, %a1, !hls.schedule !3
  ret i32 %s
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 2}
!1 = !{!2}
!2 = !{!"arg", i32 0, !"scheme", !"block", !"factor", i32 4, !"elem_bytes", i64 4}
!3 = !{!"parallel", i32 0}

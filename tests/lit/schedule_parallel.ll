; hls-schedule-annotate, parallel mode: every intercepted access of a Proven
; group gets !{!"parallel", i32 <group>}; nothing else is annotated and no
; remark is emitted. A <4 x i32> load is one instruction, so one directive.
; RUN: opt -load-pass-plugin %plugin -passes=hls-schedule-annotate -S %s | FileCheck %s
; RUN: opt -load-pass-plugin %plugin -passes=hls-schedule-annotate \
; RUN:     -pass-remarks-analysis=hls-spatial-verify -disable-output %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=REMARK --allow-empty

; CHECK-LABEL: define i32 @sum()
; CHECK:       %v0 = load i32, ptr %p0, align 4, !hls.schedule ![[G0:[0-9]+]]
; CHECK:       %i1 = or disjoint i64 %i, 1{{$}}
; CHECK:       %v1 = load i32, ptr %p1, align 4, !hls.schedule ![[G0]]
; CHECK:       %v2 = load i32, ptr %p2, align 4, !hls.schedule ![[G0]]
; CHECK:       %v3 = load i32, ptr %p3, align 4, !hls.schedule ![[G0]]
; CHECK:       %acc.1 = add i32 %acc, %v0{{$}}
; CHECK:       br i1 %cmp, label %loop, label %exit{{$}}

; CHECK-LABEL: define <4 x i32> @vload(i64 %i)
; CHECK:       %v = load <4 x i32>, ptr %p, align 16, !hls.schedule ![[G0]]

; CHECK-DAG:   ![[G0]] = !{!"parallel", i32 0}
; CHECK-NOT:   sequential

; REMARK-NOT:  remark

@A = global [64 x i32] zeroinitializer, !hls.partition !0

define i32 @sum() {
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

define <4 x i32> @vload(i64 %i) {
entry:
  %p = getelementptr inbounds [64 x i32], ptr @A, i64 0, i64 %i
  %v = load <4 x i32>, ptr %p, align 16
  ret <4 x i32> %v
}

!0 = !{!"scheme", !"cyclic", !"factor", i32 4}

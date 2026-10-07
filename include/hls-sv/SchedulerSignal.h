//===- SchedulerSignal.h - M(S) and the !hls.schedule interface -*- C++ -*-===//
//
// HLS Spatial Verification Framework
//
// Paper §5 (permission mapping M(S)) and Fig. 1 ("→ HLS scheduler"). This
// module is the boundary between the verifier and a downstream HLS scheduler:
//
//   * the verification status S and the permission mapping M(S);
//   * the transform pass `hls-schedule-annotate`, which writes the verdicts of
//     SpatialVerificationAnalysis into the IR as !hls.schedule metadata (the
//     analysis itself never mutates IR);
//   * the reader `getScheduleDirective`, which a scheduler uses to read them.
//
// ---------------------------------------------------------------------------
// !hls.schedule metadata schema
// ---------------------------------------------------------------------------
//   Attached to every memory instruction intercepted by the analysis (loads,
//   stores, atomics, and calls with memory effects):
//
//     parallel:    !{!"parallel",   i32 <group>}
//     sequential:  !{!"sequential", i32 <group>, i32 <issue_slot>, !"<status>"}
//
//   <group>      Index of the cfu(E, A, N) group (one per scheduling region
//                and array, plus one per region for unresolved accesses), in
//                the order printed by print<hls-spatial-verify>. Indices are
//                per function.
//   <issue_slot> Program-order position of the instruction among the
//                distinct instructions of its group, counting from 0: the
//                cycle offset to which the MUX/stall logic serialises it.
//                A vector access occupies one slot, whatever its lane count.
//   <status>     Why the group is serialised: "Refuted", "Unknown" or
//                "Timeout" (toString(VCStatus); never "Proven").
//
//   Example:
//     %v0 = load i32, ptr %p0, align 4, !hls.schedule !1
//     %v1 = load i32, ptr %p1, align 4, !hls.schedule !2
//     !1 = !{!"sequential", i32 0, i32 0, !"Refuted"}
//     !2 = !{!"sequential", i32 0, i32 1, !"Refuted"}
//
//   A memory instruction WITHOUT a (well-formed) directive has not been proven
//   conflict-free and must be scheduled Sequential. In particular, if Γ cannot
//   be built (environment error) no directive is attached at all. Use
//   `effectivePermission` to apply this rule.
//
//   The pass also emits one OptimizationRemarkAnalysis (pass name
//   "hls-spatial-verify") per Sequential group with the reason, and one for an
//   environment error. Show them with -pass-remarks-analysis=hls-spatial-verify.
//
//===----------------------------------------------------------------------===//
#ifndef HLS_SV_SCHEDULERSIGNAL_H
#define HLS_SV_SCHEDULERSIGNAL_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

#include <optional>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace hls_sv {

//===----------------------------------------------------------------------===//
// Verification status S and permission mapping M(S)
//===----------------------------------------------------------------------===//

enum class VCStatus {
  Proven,  ///< VC valid (paper: Sat). No two claims can share a bank.
  Refuted, ///< VC falsifiable (paper: Unsat). Counterexample exists.
  Unknown, ///< Solver gave up (e.g. incomplete tactic, internal error).
  Timeout  ///< Deterministic resource budget exhausted (paper: T_out).
};

enum class SchedulePermission { Parallel, Sequential };

llvm::StringRef toString(VCStatus S);
/// The status name used in the paper (Sat / Unsat / Unknown / Tout).
llvm::StringRef paperStatusName(VCStatus S);
llvm::StringRef toString(SchedulePermission P);
/// Inverse of toString(VCStatus).
std::optional<VCStatus> parseVCStatus(llvm::StringRef S);

/// M(S): Parallel iff S = Sat (VC proven valid); every other outcome takes
/// the conservative sequential fallback (MUX-driven stall logic).
SchedulePermission permissionFor(VCStatus S);

//===----------------------------------------------------------------------===//
// !hls.schedule
//===----------------------------------------------------------------------===//

inline constexpr const char *kScheduleMDName = "hls.schedule";
/// Pass name of the optimisation remarks.
inline constexpr const char *kRemarkPassName = "hls-spatial-verify";

/// A decoded !hls.schedule directive.
struct ScheduleDirective {
  SchedulePermission Permission = SchedulePermission::Sequential;
  unsigned Group = 0;
  /// Set iff Permission is Sequential.
  std::optional<unsigned> IssueSlot;
  /// Set iff Permission is Sequential; never VCStatus::Proven.
  std::optional<VCStatus> Status;
};

/// Read the !hls.schedule directive of I. Returns std::nullopt if there is
/// none or it does not follow the schema above.
std::optional<ScheduleDirective>
getScheduleDirective(const llvm::Instruction &I);

/// The permission a scheduler may use for I: the directive's permission if
/// there is one; otherwise Sequential for any instruction that may access
/// memory (not proven conflict-free) and Parallel for the rest.
SchedulePermission effectivePermission(const llvm::Instruction &I);

/// hls-schedule-annotate: attach !hls.schedule to every intercepted memory
/// instruction and report Sequential groups as optimisation remarks. Changes
/// metadata only; directives from an earlier run are replaced.
class ScheduleAnnotatePass : public llvm::PassInfoMixin<ScheduleAnnotatePass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};

} // namespace hls_sv

#endif // HLS_SV_SCHEDULERSIGNAL_H

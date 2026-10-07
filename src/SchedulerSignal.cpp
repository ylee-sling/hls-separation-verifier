//===- SchedulerSignal.cpp - M(S) and the !hls.schedule interface ---------===//
//
// HLS Spatial Verification Framework
//
//===----------------------------------------------------------------------===//

#include "hls-sv/SchedulerSignal.h"

#include "hls-sv/SpatialVerificationPass.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

#include <string>

using namespace llvm;

namespace hls_sv {

//===----------------------------------------------------------------------===//
// Status helpers and M(S)
//===----------------------------------------------------------------------===//

StringRef toString(VCStatus S) {
  switch (S) {
  case VCStatus::Proven:
    return "Proven";
  case VCStatus::Refuted:
    return "Refuted";
  case VCStatus::Unknown:
    return "Unknown";
  case VCStatus::Timeout:
    return "Timeout";
  }
  llvm_unreachable("unknown VCStatus");
}

StringRef paperStatusName(VCStatus S) {
  switch (S) {
  case VCStatus::Proven:
    return "Sat";
  case VCStatus::Refuted:
    return "Unsat";
  case VCStatus::Unknown:
    return "Unknown";
  case VCStatus::Timeout:
    return "Tout";
  }
  llvm_unreachable("unknown VCStatus");
}

StringRef toString(SchedulePermission P) {
  return P == SchedulePermission::Parallel ? "Parallel" : "Sequential";
}

std::optional<VCStatus> parseVCStatus(StringRef S) {
  for (VCStatus V : {VCStatus::Proven, VCStatus::Refuted, VCStatus::Unknown,
                     VCStatus::Timeout})
    if (S == toString(V))
      return V;
  return std::nullopt;
}

SchedulePermission permissionFor(VCStatus S) {
  // M(S) ≜ Parallel   if S = Sat
  //        Sequential if S ∈ {Unsat, Unknown, Tout}
  // Treating Unknown/Tout like Unsat is what keeps the framework sound in the
  // presence of non-linear arithmetic the oracle cannot settle.
  return S == VCStatus::Proven ? SchedulePermission::Parallel
                               : SchedulePermission::Sequential;
}

//===----------------------------------------------------------------------===//
// Reader
//===----------------------------------------------------------------------===//

static std::optional<unsigned> asU32(const MDOperand &Op) {
  const auto *CI = mdconst::dyn_extract_or_null<ConstantInt>(Op);
  if (!CI || CI->getBitWidth() != 32)
    return std::nullopt;
  return static_cast<unsigned>(CI->getZExtValue());
}

std::optional<ScheduleDirective> getScheduleDirective(const Instruction &I) {
  const MDNode *MD = I.getMetadata(kScheduleMDName);
  if (!MD || MD->getNumOperands() < 2)
    return std::nullopt;
  const auto *Kind = dyn_cast_or_null<MDString>(MD->getOperand(0).get());
  std::optional<unsigned> Group = asU32(MD->getOperand(1));
  if (!Kind || !Group)
    return std::nullopt;

  ScheduleDirective D;
  D.Group = *Group;
  if (Kind->getString() == "parallel" && MD->getNumOperands() == 2) {
    D.Permission = SchedulePermission::Parallel;
    return D;
  }
  if (Kind->getString() == "sequential" && MD->getNumOperands() == 4) {
    std::optional<unsigned> Slot = asU32(MD->getOperand(2));
    const auto *St = dyn_cast_or_null<MDString>(MD->getOperand(3).get());
    std::optional<VCStatus> Status =
        St ? parseVCStatus(St->getString()) : std::nullopt;
    // A Proven group is never serialised (M(S)); reject the contradiction.
    if (!Slot || !Status || *Status == VCStatus::Proven)
      return std::nullopt;
    D.Permission = SchedulePermission::Sequential;
    D.IssueSlot = Slot;
    D.Status = Status;
    return D;
  }
  return std::nullopt;
}

SchedulePermission effectivePermission(const Instruction &I) {
  if (std::optional<ScheduleDirective> D = getScheduleDirective(I))
    return D->Permission;
  return I.mayReadOrWriteMemory() ? SchedulePermission::Sequential
                                  : SchedulePermission::Parallel;
}

//===----------------------------------------------------------------------===//
// hls-schedule-annotate
//===----------------------------------------------------------------------===//

static MDNode *parallelDirective(LLVMContext &C, unsigned Group) {
  Type *I32 = Type::getInt32Ty(C);
  return MDTuple::get(C, {MDString::get(C, "parallel"),
                          ConstantAsMetadata::get(ConstantInt::get(I32, Group))});
}

static MDNode *sequentialDirective(LLVMContext &C, unsigned Group,
                                   unsigned Slot, VCStatus S) {
  Type *I32 = Type::getInt32Ty(C);
  return MDTuple::get(C, {MDString::get(C, "sequential"),
                          ConstantAsMetadata::get(ConstantInt::get(I32, Group)),
                          ConstantAsMetadata::get(ConstantInt::get(I32, Slot)),
                          MDString::get(C, toString(S))});
}

PreservedAnalyses ScheduleAnnotatePass::run(Function &F,
                                            FunctionAnalysisManager &FAM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  // Drop directives of an earlier run first: every directive in the output
  // must reflect the current analysis, and a stale "parallel" is unsound.
  bool Changed = false;
  for (Instruction &I : instructions(F))
    if (I.getMetadata(kScheduleMDName)) {
      I.setMetadata(kScheduleMDName, nullptr);
      Changed = true;
    }

  const SpatialVerificationResult &R =
      FAM.getResult<SpatialVerificationAnalysis>(F);
  OptimizationRemarkEmitter &ORE =
      FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  LLVMContext &C = F.getContext();

  if (!R.environmentError().empty()) {
    // No directive at all: readers fall back to Sequential for every memory
    // instruction (see effectivePermission).
    const Instruction *At = &*F.getEntryBlock().begin();
    ORE.emit([&] {
      return OptimizationRemarkAnalysis(kRemarkPassName, "EnvironmentError", At)
             << "environment error: " << R.environmentError()
             << "; every memory access Sequential";
    });
  }

  unsigned GroupIdx = 0;
  for (const GroupVerdict &G : R.groups()) {
    const unsigned Group = GroupIdx++;

    // Distinct instructions in program order (claims are recorded in visit
    // order; the lanes of a vector access are adjacent and share one slot).
    SmallVector<Instruction *, 8> Order;
    SmallPtrSet<const Instruction *, 8> Seen;
    for (const ClaimRecord &CR : G.Claims)
      if (Seen.insert(CR.Access).second)
        // The analysis result refers to instructions of F, which this
        // transform pass is allowed to modify.
        Order.push_back(const_cast<Instruction *>(CR.Access));

    const bool Sequential = G.Permission == SchedulePermission::Sequential;
    for (unsigned Slot = 0; Slot < Order.size(); ++Slot)
      Order[Slot]->setMetadata(
          kScheduleMDName, Sequential
                               ? sequentialDirective(C, Group, Slot, G.Status)
                               : parallelDirective(C, Group));
    Changed |= !Order.empty();

    if (Sequential && !Order.empty())
      ORE.emit([&] {
        return OptimizationRemarkAnalysis(kRemarkPassName, "SequentialGroup",
                                          Order.front())
               << "group " << std::to_string(Group) << " (" << G.ArrayDesc
               << "): " << toString(G.Status) << " -> Sequential, "
               << std::to_string(Order.size())
               << " access(es) serialised: " << G.Reason;
      });
  }

  if (!Changed)
    return PreservedAnalyses::all();
  // Metadata only: no instruction, block or edge was added or removed.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

} // namespace hls_sv

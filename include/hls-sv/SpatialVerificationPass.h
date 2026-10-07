//===- SpatialVerificationPass.h - CFU analysis over unrolled IR -*- C++
//-*-===//
//
// HLS Spatial Verification Framework
//
// New-pass-manager function analysis that runs after loop unrolling
// (paper Fig. 1, "HLS Unrolling Pass → Entailment Engine → T → SMT"):
//
//   1. An InstVisitor intercepts every load / store / atomic and reads the
//      flat 1-D offset off the getelementptr chain feeding it.
//   2. Each access is attached to its scheduling region (the innermost loop
//      body after unrolling, or the function top level for fully unrolled
//      code) and to its array A, whose Γ(A) instantiates bank(k, A, N).
//   3. Per (region, A): cfu(E, A, N) = ∗_i bank(𝔅(expr_i, N), A, N) is
//      translated by T into the pairwise VC matrix and discharged by Z3.
//   4. The permission mapping M(S) yields Parallel / Sequential per group,
//      which the HLS scheduler queries through the analysis result.
//
// Pipeline names (pass plugin):
//   print<hls-spatial-verify>   run the analysis and print verdicts
//   hls-spatial-verify          run the analysis (cache it for a scheduler)
//
//===----------------------------------------------------------------------===//
#ifndef HLS_SV_SPATIALVERIFICATIONPASS_H
#define HLS_SV_SPATIALVERIFICATIONPASS_H

#include "hls-sv/SMTTranslator.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace llvm {
class Function;
} // namespace llvm

namespace hls_sv {

/// One conjunct bank(k_i, A, N) of a group, in printable form. The analysis
/// result holds no Z3 objects, so it is independent of the solver context.
struct ClaimRecord {
  const llvm::Instruction *Access = nullptr;
  unsigned Lane = 0;
  bool IsWrite = false;
  std::string Offset;    ///< expr_i
  std::string BankIndex; ///< k_i = 𝔅(expr_i, N)
};

/// The verdict for one cfu(E, A, N): all accesses to array A inside one
/// scheduling region.
struct GroupVerdict {
  /// Header of the scheduling region; null for the function top level.
  const llvm::BasicBlock *RegionHeader = nullptr;
  const llvm::Value *ArrID = nullptr; ///< null for the unresolved bucket
  std::string ArrayDesc;              ///< Γ(A) in printable form
  std::vector<ClaimRecord> Claims;
  VCStatus Status = VCStatus::Unknown;
  SchedulePermission Permission = SchedulePermission::Sequential;
  std::string Reason;
  std::vector<PairEntry> Matrix;
  std::string SMTLib; ///< ¬VC as SMT-LIB2 (only with -hls-sv-dump-vc)
};

class SpatialVerificationResult {
public:
  llvm::ArrayRef<GroupVerdict> groups() const { return Groups; }

  /// Scheduler query: may this memory instruction be issued concurrently
  /// with the other accesses of its group? Instructions that are not
  /// intercepted memory accesses are unconstrained (Parallel).
  SchedulePermission permissionFor(const llvm::Instruction *I) const;

  /// True iff every group may be synthesised with parallel SRAM paths.
  bool allParallel() const;

  /// Non-empty if Γ could not be built; every access is then Sequential.
  const std::string &environmentError() const { return EnvError; }

  void print(llvm::raw_ostream &OS, const llvm::Function &F) const;

private:
  friend class SpatialVerificationAnalysis;

  std::vector<GroupVerdict> Groups;
  llvm::DenseMap<const llvm::Instruction *, SchedulePermission> PerAccess;
  std::string EnvError;
};

/// Function analysis computing CFU verdicts for every scheduling region.
class SpatialVerificationAnalysis
    : public llvm::AnalysisInfoMixin<SpatialVerificationAnalysis> {
  friend llvm::AnalysisInfoMixin<SpatialVerificationAnalysis>;
  static llvm::AnalysisKey Key;

public:
  using Result = SpatialVerificationResult;
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
};

/// print<hls-spatial-verify>
class SpatialVerificationPrinterPass
    : public llvm::PassInfoMixin<SpatialVerificationPrinterPass> {
public:
  explicit SpatialVerificationPrinterPass(llvm::raw_ostream &OS) : OS(OS) {}
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }

private:
  llvm::raw_ostream &OS;
};

} // namespace hls_sv

#endif // HLS_SV_SPATIALVERIFICATIONPASS_H

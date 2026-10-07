//===- SMTTranslator.h - T(π ⊢ cfu) → Z3 verification condition -*- C++ -*-===//
//
// HLS Spatial Verification Framework
//
// Paper §5 "Translating Spatial Disjointness to SMT".
//
//   T(π ⊢ cfu(K, A, N)) ≜ π ⇒ ⋀_{1≤i<j≤U} (k_i ≠ k_j)
//
// Each claim carries a guard A_i — a necessary condition for access i to
// execute (dominating branch edges, llvm.assume facts, poison-freedom of its
// address cone, 0 ≤ expr_i < M). The matrix entry for (i, j) is therefore
// (A_i ∧ A_j) ⇒ k_i ≠ k_j, which is exactly the paper's k_i ≠ k_j when both
// accesses are unconditional.
//
// IMPORTANT — validity vs. satisfiability. The scheduler may run U accesses
// in parallel only if the VC is *valid* (true under every assignment of the
// SSA symbols). Asserting the VC and asking Z3 for `sat` would only show that
// *some* assignment separates the banks, which is unsound. We therefore
// assert ¬VC = π ∧ ⋁(A_i ∧ A_j ∧ k_i = k_j) and require `unsat`:
//
//     paper status "Sat"   (VC valid)       ⇔  Z3(¬VC) = unsat  → Proven
//     paper status "Unsat" (VC falsifiable) ⇔  Z3(¬VC) = sat    → Refuted
//
// A Z3 model of ¬VC is a concrete counterexample: an assignment under which
// two accesses claim the same bank, i.e. bank(k) * bank(k) ⇒ ⊥.
//
// Arithmetic is encoded in QF_BV at the LLVM bit widths, so wrap-around,
// udiv/sdiv and urem/srem are modelled exactly.
//
//===----------------------------------------------------------------------===//
#ifndef HLS_SV_SMTTRANSLATOR_H
#define HLS_SV_SMTTRANSLATOR_H

#include "hls-sv/BankPredicate.h"
#include "hls-sv/MemoryModel.h"
#include "hls-sv/SchedulerSignal.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Value.h"

#include <z3++.h>

#include <string>
#include <utility>
#include <vector>

namespace llvm {
class APInt;
class BinaryOperator;
class DataLayout;
class Function;
class ScalarEvolution;
class Type;
} // namespace llvm

namespace hls_sv {

// VCStatus, SchedulePermission and the permission mapping M(S) live in
// hls-sv/SchedulerSignal.h.

struct TranslatorConfig {
  /// Z3 resource budget per check. Resource units are counted
  /// deterministically for a fixed Z3 version, which makes T_out
  /// reproducible across machines, unlike a wall-clock timeout.
  unsigned RLimit = 5'000'000;
  /// Optional wall-clock safety net (ms, 0 = off). Not deterministic.
  unsigned WallClockMs = 0;
  /// When the conjoined VC is not proven, check each pair on its own to fill
  /// the conflict matrix (and possibly upgrade Unknown/Timeout to Proven).
  bool RefinePairs = true;
  /// Groups with more claims than this take the fallback without a query.
  unsigned MaxClaims = 256;
};

//===----------------------------------------------------------------------===//
// SymbolicEvaluator — ⟦·⟧ over the Monotonic Store
//===----------------------------------------------------------------------===//

/// Translates integer SSA values into Z3 terms, memoising every register in
/// the MonotonicStore (each register is evaluated once and never rebound).
/// Values whose semantics are not interpreted (φ-nodes, loads, calls, freeze)
/// become opaque symbols constrained only by sound invariants (SCEV ranges,
/// known bits). Havocking a value over-approximates its behaviours, which can
/// only add counterexamples, never hide one.
class SymbolicEvaluator {
public:
  SymbolicEvaluator(z3::context &Ctx, MonotonicStore &Store,
                    const llvm::Function &F, const llvm::DataLayout &DL,
                    llvm::ScalarEvolution *SE);

  /// ⟦V⟧: Bool for i1, a bit-vector of V's width for other integers.
  z3::expr evaluate(const llvm::Value *V);
  /// ⟦V⟧ as a getelementptr index of width W (GEP sign-extends or truncates).
  z3::expr asIndex(const llvm::Value *V, unsigned W);
  /// ⟦C⟧ as a Bool (branch, switch-free select and assume conditions).
  z3::expr asBool(const llvm::Value *C);

  /// Pure invariants of the opaque symbols created so far. They hold on
  /// every UB-free trace and are conjoined into π.
  llvm::ArrayRef<z3::expr> invariants() const { return Invariants; }

  /// Poison-freedom facts (nsw/nuw/exact/disjoint/nneg, shift amounts,
  /// non-zero divisors) of every interpreted instruction in V's cone. Poison
  /// reaching an address or a branch is UB, so these facts hold whenever the
  /// consuming instruction executes.
  void collectDefinednessFacts(const llvm::Value *V,
                               std::vector<z3::expr> &Out) const;

  z3::expr bv(const llvm::APInt &A);
  z3::expr bv(uint64_t N, unsigned W) { return Ctx.bv_val(N, W); }

  /// Printable operand name (%4, %idx.1, @A) consistent with the IR printer.
  std::string displayName(const llvm::Value *V);

private:
  z3::expr evaluateImpl(const llvm::Value *V, unsigned Depth);
  /// Interpret I's semantics; sets Opaque when I is outside the fragment.
  z3::expr interpret(const llvm::Instruction *I, unsigned Depth, bool &Opaque);
  z3::expr opaqueSymbol(const llvm::Value *V);
  z3::expr freshSymbol(const llvm::Type *Ty, llvm::StringRef Hint);
  void addValueInvariants(const llvm::Value *V, const z3::expr &X);
  void recordPoisonFacts(const llvm::BinaryOperator *BO, const z3::expr &A,
                         const z3::expr &B, const z3::expr &R);
  void addDefinedness(const llvm::Instruction *I, z3::expr Fact);

  z3::context &Ctx;
  MonotonicStore &Store;
  const llvm::DataLayout &DL;
  llvm::ScalarEvolution *SE;
  llvm::ModuleSlotTracker MST;

  std::vector<z3::expr> Invariants;
  llvm::DenseMap<const llvm::Instruction *, std::vector<z3::expr>> Definedness;
  llvm::SmallPtrSet<const llvm::Value *, 32> OpaqueRegs;
  llvm::SmallPtrSet<const llvm::Value *, 32> InProgress;
  unsigned FreshCounter = 0;

  /// Deeper cones are havocked (sound) rather than recursed into.
  static constexpr unsigned MaxDepth = 256;
};

//===----------------------------------------------------------------------===//
// Claims, the VC matrix and results
//===----------------------------------------------------------------------===//

/// One conjunct bank(k_i, A, N) of cfu(E, A, N), with k_i = 𝔅(expr_i, N).
struct BankClaim {
  BankPredicate Pred;              ///< bank(k_i, A, N)
  z3::expr Offset;                 ///< expr_i, flat element offset
  z3::expr Guard;                  ///< A_i, necessary for access i to run
  const llvm::Instruction *Access; ///< the intercepted load/store
  unsigned Lane;                   ///< lane of a vector access (else 0)
};

enum class PairClass {
  Separated, ///< Decided without the oracle: k_i ≠ k_j simplifies to true,
             ///< different arrays, or the guards are contradictory.
  Collapse,  ///< k_i and k_j are the same term: bank(k) * bank(k).
  Symbolic   ///< Needs the SMT oracle.
};

/// One entry (i < j) of the pairwise VC matrix.
struct PairEntry {
  unsigned I = 0, J = 0;
  PairClass Class = PairClass::Symbolic;
  VCStatus Status = VCStatus::Unknown; ///< Proven ⇔ k_i ≠ k_j holds under π.
  bool Decided = false;
  std::string Witness; ///< Counterexample (Refuted) or solver reason.
};

struct VerificationCondition {
  z3::expr Pi;                   ///< π
  z3::expr Formula;              ///< T(π ⊢ cfu(K, A, N))
  std::vector<PairEntry> Matrix; ///< Upper triangle, row-major.
  /// (matrix index, (A_i ∧ A_j) ⇒ k_i ≠ k_j) for every non-separated pair.
  std::vector<std::pair<size_t, z3::expr>> Obligations;
};

struct VCResult {
  VCStatus Status = VCStatus::Unknown;
  SchedulePermission Permission = SchedulePermission::Sequential;
  std::string Reason;
  std::vector<PairEntry> Matrix;
};

//===----------------------------------------------------------------------===//
// SMTTranslator
//===----------------------------------------------------------------------===//

class SMTTranslator {
public:
  SMTTranslator(const llvm::Function &F, const llvm::DataLayout &DL,
                llvm::ScalarEvolution *SE, TranslatorConfig Cfg = {});

  SMTTranslator(const SMTTranslator &) = delete;
  SMTTranslator &operator=(const SMTTranslator &) = delete;

  z3::context &context() { return Ctx; }
  MonotonicStore &store() { return Store; }
  SymbolicEvaluator &evaluator() { return Eval; }
  const TranslatorConfig &config() const { return Cfg; }

  /// The translation function T: reduce π ⊢ cfu(K, A, N) to the pairwise
  /// inequality matrix, resolving syntactically decidable pairs on the way.
  VerificationCondition translate(const z3::expr &Pi,
                                  llvm::ArrayRef<BankClaim> K);

  /// Submit the VC to Z3 and apply the permission mapping M(S).
  VCResult discharge(const VerificationCondition &VC,
                     llvm::ArrayRef<BankClaim> K);

  /// SMT-LIB2 text of the query actually sent to Z3 (¬VC).
  std::string toSMTLib(const VerificationCondition &VC);

private:
  void configureSolver();
  void resetSolver();
  VCStatus classifyUnknown();
  void refinePairs(const VerificationCondition &VC, llvm::ArrayRef<BankClaim> K,
                   VCResult &R);
  std::string witness(const z3::model &M, const BankClaim &A,
                      const BankClaim &B);

  TranslatorConfig Cfg;
  z3::context Ctx; // declared first among Z3 objects: destroyed last
  z3::solver Solver;
  MonotonicStore Store;
  SymbolicEvaluator Eval;
};

} // namespace hls_sv

#endif // HLS_SV_SMTTRANSLATOR_H

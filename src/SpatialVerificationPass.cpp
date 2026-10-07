//===- SpatialVerificationPass.cpp - CFU analysis over unrolled IR --------===//
//
// HLS Spatial Verification Framework
//
//===----------------------------------------------------------------------===//

#include "hls-sv/SpatialVerificationPass.h"

#include "hls-sv/BankPredicate.h"
#include "hls-sv/MemoryModel.h"
#include "hls-sv/SMTTranslator.h"
#include "hls-sv/SchedulerSignal.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Operator.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace hls_sv;

//===----------------------------------------------------------------------===//
// Options
//===----------------------------------------------------------------------===//

static cl::list<std::string> PartitionOverrides(
    "hls-sv-partition",
    cl::desc("Gamma override <name>:<scheme>:<N>[:<M>[:<elem_bytes>]]"));

static cl::opt<unsigned>
    RLimitOpt("hls-sv-rlimit", cl::init(5'000'000),
              cl::desc("Z3 resource budget per check (deterministic T_out)"));

static cl::opt<unsigned>
    TimeoutMsOpt("hls-sv-timeout-ms", cl::init(0),
                 cl::desc("Optional wall-clock limit per check in ms "
                          "(non-deterministic; 0 = off)"));

static cl::opt<bool> RefinePairsOpt(
    "hls-sv-refine-pairs", cl::init(true),
    cl::desc("Fill the pairwise conflict matrix when the VC is not proven"));

static cl::opt<unsigned> MaxClaimsOpt(
    "hls-sv-max-claims", cl::init(256),
    cl::desc("Groups with more bank claims take the sequential fallback"));

static cl::opt<bool> DumpVCOpt("hls-sv-dump-vc", cl::init(false),
                               cl::desc("Print the SMT-LIB2 query per group"));

static cl::opt<bool> CheckGammaOpt(
    "hls-sv-check-gamma", cl::init(true),
    cl::desc("Check arr(A) = *_k bank(k, A, N) for every directive"));

static cl::opt<bool> PrintAtEndOpt(
    "hls-sv-print-at-end", cl::init(false),
    cl::desc("Print verdicts at the end of the default optimisation "
             "pipeline, i.e. after every unrolling pass"));

namespace {

//===----------------------------------------------------------------------===//
// GEP interception
//===----------------------------------------------------------------------===//

/// Flat 1-D byte offset of a pointer, read off the getelementptr chain that
/// computes it:  Base + Const + Σ Scale_v · v  (at the GEP index width).
struct FlatOffset {
  const Value *Base = nullptr;
  APInt Const;
  SmallVector<std::pair<const Value *, APInt>, 4> Terms;
};

/// A memory operation intercepted by the visitor.
struct MemAccess {
  const Instruction *Inst = nullptr;
  const Loop *Region = nullptr; ///< innermost loop after unrolling
  bool IsWrite = false;
  const ArrayInfo *Array = nullptr;   ///< null ⇒ unresolved
  std::string Unresolved;             ///< why the access could not be mapped
  std::optional<z3::expr> Offset;     ///< expr_i, flat element offset
  unsigned Lanes = 1;                 ///< elements touched in the same cycle
  SmallVector<const Value *, 4> Cone; ///< SSA indices feeding expr_i
};

class AccessCollector : public InstVisitor<AccessCollector> {
public:
  AccessCollector(StaticHardwareEnv &Env, const DataLayout &DL,
                  const LoopInfo &LI, SMTTranslator &T)
      : Env(Env), DL(DL), LI(LI), T(T) {}

  void visitLoadInst(LoadInst &I) {
    record(I, I.getPointerOperand(), I.getType(), /*IsWrite=*/false);
  }
  void visitStoreInst(StoreInst &I) {
    record(I, I.getPointerOperand(), I.getValueOperand()->getType(),
           /*IsWrite=*/true);
  }
  void visitAtomicRMWInst(AtomicRMWInst &I) {
    record(I, I.getPointerOperand(), I.getValOperand()->getType(), true);
  }
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {
    record(I, I.getPointerOperand(), I.getCompareOperand()->getType(), true);
  }
  void visitCallBase(CallBase &CB) {
    if (auto *AS = dyn_cast<AssumeInst>(&CB)) {
      Assumes.push_back(AS); // pure fact, contributes to path conditions
      return;
    }
    if (CB.isDebugOrPseudoInst() || CB.isLifetimeStartOrEnd() ||
        isa<NoAliasScopeDeclInst>(CB) || CB.doesNotAccessMemory())
      return;
    // A call that may touch memory cannot be mapped to a bank: it may alias
    // any of them, so its region is serialised.
    MemAccess A = makeAccess(CB, /*IsWrite=*/!CB.onlyReadsMemory());
    A.Unresolved = "call with memory effects";
    Accesses.push_back(std::move(A));
  }

  std::vector<MemAccess> Accesses;
  std::vector<const AssumeInst *> Assumes;

private:
  MemAccess makeAccess(Instruction &I, bool IsWrite) const {
    MemAccess A;
    A.Inst = &I;
    A.Region = LI.getLoopFor(I.getParent());
    A.IsWrite = IsWrite;
    return A;
  }

  bool decompose(const Value *Ptr, unsigned IdxW, FlatOffset &FO,
                 std::string &Why) const;
  void record(Instruction &I, Value *Ptr, Type *AccessTy, bool IsWrite);

  StaticHardwareEnv &Env;
  const DataLayout &DL;
  const LoopInfo &LI;
  SMTTranslator &T;
};

bool AccessCollector::decompose(const Value *Ptr, unsigned IdxW, FlatOffset &FO,
                                std::string &Why) const {
  FO.Const = APInt(IdxW, 0);
  const Value *Cur = Ptr;
  for (unsigned Step = 0; Step < 64; ++Step) {
    Cur = Cur->stripPointerCasts();
    if (const auto *GEP = dyn_cast<GEPOperator>(Cur)) {
      if (GEP->getType()->isVectorTy()) {
        Why = "vector getelementptr (gather/scatter)";
        return false;
      }
      // LLVM's own GEP semantics give the flat offset; multi-dimensional
      // C/C++ subscripts were already lowered into this arithmetic.
#if LLVM_VERSION_MAJOR >= 20
      SmallMapVector<Value *, APInt, 4> Vars;
#else
      MapVector<Value *, APInt> Vars;
#endif
      APInt C(IdxW, 0);
      if (!GEP->collectOffset(DL, IdxW, Vars, C)) {
        Why = "getelementptr offset is not decomposable";
        return false;
      }
      FO.Const += C;
      for (const auto &KV : Vars)
        FO.Terms.emplace_back(KV.first, KV.second);
      Cur = GEP->getPointerOperand();
      continue;
    }
    if (isa<GlobalVariable, AllocaInst, Argument>(Cur)) {
      FO.Base = Cur;
      return true;
    }
    if (isa<PHINode, SelectInst>(Cur))
      Why = "pointer merges several objects (phi/select)";
    else if (isa<LoadInst>(Cur))
      Why = "pointer loaded from memory";
    else
      Why = "pointer is not a getelementptr chain over a static allocation";
    return false;
  }
  Why = "getelementptr chain too deep";
  return false;
}

void AccessCollector::record(Instruction &I, Value *Ptr, Type *AccessTy,
                             bool IsWrite) {
  MemAccess A = makeAccess(I, IsWrite);
  auto Fail = [&](const Twine &Why) {
    A.Array = nullptr;
    A.Unresolved = Why.str();
    Accesses.push_back(std::move(A));
  };

  const unsigned IdxW = DL.getIndexTypeSizeInBits(Ptr->getType());
  FlatOffset FO;
  std::string Why;
  if (!decompose(Ptr, IdxW, FO, Why))
    return Fail(Why);

  const ArrayInfo *Arr = Env.lookupOrImplicit(FO.Base, DL);
  if (!Arr)
    return Fail("underlying object is not a static allocation");
  A.Array = Arr;

  if (isa<ScalableVectorType>(AccessTy))
    return Fail("scalable vector access");
  unsigned Lanes = 1;
  Type *LaneTy = AccessTy;
  if (auto *VT = dyn_cast<FixedVectorType>(AccessTy)) {
    Lanes = VT->getNumElements();
    LaneTy = VT->getElementType();
  }
  const uint64_t LaneBytes = DL.getTypeStoreSize(LaneTy).getFixedValue();

  // Byte offset → flat element offset; only exact divisions are accepted.
  const int64_t EB = static_cast<int64_t>(Arr->ElemBytes);
  bool Exact = FO.Const.srem(EB) == 0;
  for (const auto &Term : FO.Terms)
    Exact &= Term.second.srem(EB) == 0;

  if (Arr->Factor != 1) {
    if (!Exact)
      return Fail("getelementptr offset is not a whole number of elements");
    if (LaneBytes != Arr->ElemBytes)
      return Fail("access width differs from the element size (may straddle "
                  "banks)");
    A.Lanes = Lanes;
  }
  // A monolithic SRAM (N = 1) is a single bank: one claim, whatever width.

  if (Exact) {
    SymbolicEvaluator &E = T.evaluator();
    z3::expr Off = E.bv(FO.Const.sdiv(EB));
    for (const auto &[V, Scale] : FO.Terms) {
      Off = Off + E.asIndex(V, IdxW) * E.bv(Scale.sdiv(EB));
      A.Cone.push_back(V);
    }
    Off = Off.simplify();
    A.Offset = Off;

    // Dual state: the pointer register holds an address,
    //   s(%ptr) = α + expr_i  ∈ L ⊂ Val.
    if (MonotonicStore::isRegister(Ptr)) {
      hls_sv::Loc L = hls_sv::Loc::make(
          (T.context().bv_val(Arr->Base, IdxW) + Off).simplify(), Arr->ArrID);
      [[maybe_unused]] MonotonicStore::BindStatus St = T.store().bind(Ptr, L);
      assert(St != MonotonicStore::BindStatus::ImmutabilityViolation &&
             "pointer register rebound to a different address");
    }
  }
  Accesses.push_back(std::move(A));
}

//===----------------------------------------------------------------------===//
// Path conditions
//===----------------------------------------------------------------------===//

/// Builds necessary execution conditions from dominating branch edges.
class GuardBuilder {
public:
  GuardBuilder(SMTTranslator &T, const DominatorTree &DT, const LoopInfo &LI)
      : T(T), DT(DT), LI(LI) {}

  /// Conjunction of the branch edges that dominate BB inside Region. Each
  /// conjunct holds whenever BB executes in the current iteration of Region.
  z3::expr blockGuard(const BasicBlock *BB, const Loop *Region) {
    auto It = Cache.find(BB);
    if (It != Cache.end())
      return It->second;
    z3::expr G = walk(BB, Region);
    Cache.try_emplace(BB, G);
    return G;
  }

  /// Region-level part of π: edges that dominate the region header.
  z3::expr regionCondition(const Loop *Region) {
    if (!Region)
      return T.context().bool_val(true);
    return walk(Region->getHeader(), nullptr);
  }

private:
  z3::expr walk(const BasicBlock *BB, const Loop *Region) {
    z3::expr_vector Parts(T.context());
    const DomTreeNode *N = DT.getNode(BB);
    for (N = N ? N->getIDom() : nullptr; N; N = N->getIDom()) {
      const BasicBlock *D = N->getBlock();
      if (Region && !Region->contains(D))
        break; // outside the region: those edges belong to π
      const Loop *LD = LI.getLoopFor(D);
      if (LD && !LD->contains(BB))
        continue; // decided in an earlier iteration of a sibling/inner loop
      if (std::optional<z3::expr> F = edgeFact(D, BB))
        Parts.push_back(*F);
    }
    return Parts.size() == 0 ? T.context().bool_val(true) : z3::mk_and(Parts);
  }

  std::optional<z3::expr> edgeFact(const BasicBlock *D, const BasicBlock *BB) {
    SymbolicEvaluator &E = T.evaluator();
    const Instruction *Term = D->getTerminator();

    if (const auto *Br = dyn_cast<BranchInst>(Term)) {
      if (!Br->isConditional() || Br->getSuccessor(0) == Br->getSuccessor(1))
        return std::nullopt;
      for (unsigned S = 0; S < 2; ++S) {
        if (!DT.dominates(BasicBlockEdge(D, Br->getSuccessor(S)), BB))
          continue;
        z3::expr C = E.asBool(Br->getCondition());
        z3::expr Fact = S == 0 ? C : !C;
        // Branching on poison is UB: the condition's cone is well defined.
        std::vector<z3::expr> Defs;
        E.collectDefinednessFacts(Br->getCondition(), Defs);
        for (const z3::expr &Df : Defs)
          Fact = Fact && Df;
        return Fact;
      }
      return std::nullopt;
    }

    if (const auto *SI = dyn_cast<SwitchInst>(Term)) {
      const Value *Cond = SI->getCondition();
      if (!Cond->getType()->isIntegerTy() || Cond->getType()->isIntegerTy(1))
        return std::nullopt;
      z3::expr X = E.evaluate(Cond);
      for (const auto &Case : SI->cases())
        if (DT.dominates(BasicBlockEdge(D, Case.getCaseSuccessor()), BB))
          return z3::expr(X == E.bv(Case.getCaseValue()->getValue()));
      if (DT.dominates(BasicBlockEdge(D, SI->getDefaultDest()), BB)) {
        z3::expr_vector Ne(T.context());
        for (const auto &Case : SI->cases())
          Ne.push_back(X != E.bv(Case.getCaseValue()->getValue()));
        if (Ne.size() != 0)
          return z3::mk_and(Ne);
      }
    }
    return std::nullopt;
  }

  SMTTranslator &T;
  const DominatorTree &DT;
  const LoopInfo &LI;
  DenseMap<const BasicBlock *, z3::expr> Cache;
};

//===----------------------------------------------------------------------===//
// Per-function driver
//===----------------------------------------------------------------------===//

class Analyzer {
public:
  Analyzer(Function &F, const DominatorTree &DT, const LoopInfo &LI,
           StaticHardwareEnv &Env, SMTTranslator &T)
      : F(F), DT(DT), LI(LI), Env(Env), T(T) {}

  void run();

  std::vector<GroupVerdict> Groups;
  DenseMap<const Instruction *, SchedulePermission> PerAccess;
  std::string EnvError;

private:
  z3::expr accessGuard(const MemAccess &A, ArrayRef<const AssumeInst *> Assumes,
                       GuardBuilder &GB);
  void publish(GroupVerdict GV);

  Function &F;
  const DominatorTree &DT;
  const LoopInfo &LI;
  StaticHardwareEnv &Env;
  SMTTranslator &T;
};

z3::expr Analyzer::accessGuard(const MemAccess &A,
                               ArrayRef<const AssumeInst *> Assumes,
                               GuardBuilder &GB) {
  SymbolicEvaluator &E = T.evaluator();
  const BasicBlock *BB = A.Inst->getParent();
  z3::expr G = GB.blockGuard(BB, A.Region);

  // llvm.assume facts that executed before the access in this iteration.
  for (const AssumeInst *AS : Assumes) {
    const Loop *LA = LI.getLoopFor(AS->getParent());
    if ((LA && !LA->contains(BB)) || !DT.dominates(AS, A.Inst))
      continue;
    G = G && E.asBool(AS->getArgOperand(0));
  }

  // Poison-freedom of the address cone (poison address ⇒ UB).
  std::vector<z3::expr> Defs;
  for (const Value *V : A.Cone)
    E.collectDefinednessFacts(V, Defs);
  for (const z3::expr &D : Defs)
    G = G && D;
  return G;
}

void Analyzer::publish(GroupVerdict GV) {
  for (const ClaimRecord &C : GV.Claims) {
    if (GV.Permission == SchedulePermission::Sequential)
      PerAccess[C.Access] = SchedulePermission::Sequential;
    else
      PerAccess.try_emplace(C.Access, SchedulePermission::Parallel);
  }
  Groups.push_back(std::move(GV));
}

void Analyzer::run() {
  // Γ consistency: arr(A) ≜ ∗_{k<N} bank(k, A, N) must compose without
  // collapse and agree with 𝔅 for every partition directive.
  if (CheckGammaOpt)
    for (const auto &A : Env.entries())
      if (A->Explicit)
        if (Error E = verifyCompositionRule(T.context(), *A)) {
          EnvError = llvm::toString(std::move(E));
          return;
        }

  const DataLayout &DL = F.getParent()->getDataLayout();
  AccessCollector Collector(Env, DL, LI, T);
  for (BasicBlock &BB : F)
    if (DT.isReachableFromEntry(&BB))
      Collector.visit(BB);

  // One cfu(E, A, N) per (scheduling region, array).
  MapVector<std::pair<const Loop *, const ArrayInfo *>,
            SmallVector<unsigned, 8>>
      ByGroup;
  MapVector<const Loop *, SmallVector<unsigned, 4>> UnresolvedByRegion;
  for (unsigned I = 0; I < Collector.Accesses.size(); ++I) {
    const MemAccess &A = Collector.Accesses[I];
    if (A.Array)
      ByGroup[std::make_pair(A.Region, A.Array)].push_back(I);
    else
      UnresolvedByRegion[A.Region].push_back(I);
  }

  GuardBuilder GB(T, DT, LI);
  z3::context &Ctx = T.context();

  for (auto &[Key, Members] : ByGroup) {
    const Loop *Region = Key.first;
    const ArrayInfo *Arr = Key.second;

    GroupVerdict GV;
    GV.RegionHeader = Region ? Region->getHeader() : nullptr;
    GV.ArrID = Arr->ArrID;
    {
      raw_string_ostream OS(GV.ArrayDesc);
      Arr->print(OS);
    }

    // Instantiate bank(k_i, A, N) for every claim, k_i = 𝔅(expr_i, N).
    std::vector<BankClaim> Claims;
    for (unsigned Idx : Members) {
      const MemAccess &A = Collector.Accesses[Idx];
      const z3::expr Base = accessGuard(A, Collector.Assumes, GB);

      auto AddClaim = [&](const z3::expr &Off, const z3::expr &K,
                          z3::expr Guard, unsigned Lane) {
        Claims.push_back(BankClaim{BankPredicate{Arr, K}, Off, Guard.simplify(),
                                   A.Inst, Lane});
        GV.Claims.push_back(ClaimRecord{A.Inst, Lane, A.IsWrite,
                                        Off.to_string(), K.to_string()});
      };

      if (Arr->Factor == 1) {
        // Monolithic SRAM: bank(0, A, 1) is the whole array.
        const unsigned W = A.Offset ? A.Offset->get_sort().bv_size() : 64;
        z3::expr Off = A.Offset ? *A.Offset : Ctx.bv_val(0, W);
        z3::expr Guard = Base;
        if (A.Offset && Arr->Capacity)
          Guard = Guard && z3::ult(Off, Ctx.bv_val(*Arr->Capacity, W));
        AddClaim(Off, Ctx.bv_val(0, W), Guard, 0);
        continue;
      }

      assert(A.Offset && "partitioned accesses always carry expr_i");
      const unsigned W = A.Offset->get_sort().bv_size();
      for (unsigned L = 0; L < A.Lanes; ++L) {
        z3::expr Off = (*A.Offset + Ctx.bv_val(L, W)).simplify();
        z3::expr Guard = Base;
        // An executed access lies inside its object (UB otherwise), so
        // 0 ≤ expr_i < M is a necessary condition for it to run.
        if (Arr->Capacity)
          Guard = Guard && z3::ult(Off, Ctx.bv_val(*Arr->Capacity, W));
        AddClaim(Off, bankIndex(*Arr, Off).simplify(), Guard, L);
      }
    }

    // π: region-level path condition ∧ invariants of the opaque symbols.
    z3::expr_vector PiParts(Ctx);
    PiParts.push_back(GB.regionCondition(Region));
    for (const z3::expr &Inv : T.evaluator().invariants())
      PiParts.push_back(Inv);
    const z3::expr Pi = z3::mk_and(PiParts);

    auto Taint = UnresolvedByRegion.find(Region);
    if (Taint != UnresolvedByRegion.end()) {
      const MemAccess &U = Collector.Accesses[Taint->second.front()];
      GV.Status = VCStatus::Unknown;
      GV.Reason = "region also contains an unresolved memory access (" +
                  U.Unresolved + ") that may alias any bank";
    } else {
      VerificationCondition VC = T.translate(Pi, Claims);
      VCResult Res = T.discharge(VC, Claims);
      GV.Status = Res.Status;
      GV.Reason = std::move(Res.Reason);
      GV.Matrix = std::move(Res.Matrix);
      if (DumpVCOpt)
        GV.SMTLib = T.toSMTLib(VC);
    }
    GV.Permission = permissionFor(GV.Status);
    publish(std::move(GV));
  }

  // Accesses that could not be mapped to Γ are serialised unconditionally.
  for (auto &[Region, Members] : UnresolvedByRegion) {
    GroupVerdict GV;
    GV.RegionHeader = Region ? Region->getHeader() : nullptr;
    GV.ArrayDesc = "<unresolved base>";
    for (unsigned Idx : Members) {
      const MemAccess &A = Collector.Accesses[Idx];
      GV.Claims.push_back(ClaimRecord{A.Inst, 0, A.IsWrite, "?", "?"});
    }
    GV.Status = VCStatus::Unknown;
    GV.Reason = "cannot map to Gamma: " +
                Collector.Accesses[Members.front()].Unresolved;
    GV.Permission = SchedulePermission::Sequential;
    publish(std::move(GV));
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// Analysis / result
//===----------------------------------------------------------------------===//

namespace hls_sv {

AnalysisKey SpatialVerificationAnalysis::Key;

SpatialVerificationResult
SpatialVerificationAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
  SpatialVerificationResult R;
  if (F.isDeclaration())
    return R;

  const DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  const LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  const DataLayout &DL = F.getParent()->getDataLayout();

  // Γ: metadata plus command-line overrides.
  std::vector<GammaOverride> Overrides;
  for (const std::string &Text : PartitionOverrides) {
    Expected<GammaOverride> O = parseGammaOverride(Text);
    if (!O) {
      R.EnvError = llvm::toString(O.takeError());
      return R;
    }
    Overrides.push_back(std::move(*O));
  }
  Expected<StaticHardwareEnv> EnvOr = StaticHardwareEnv::build(F, Overrides);
  if (!EnvOr) {
    R.EnvError = llvm::toString(EnvOr.takeError());
    return R;
  }

  TranslatorConfig Cfg;
  Cfg.RLimit = RLimitOpt;
  Cfg.WallClockMs = TimeoutMsOpt;
  Cfg.RefinePairs = RefinePairsOpt;
  Cfg.MaxClaims = MaxClaimsOpt;

  // Z3's C++ API reports errors by exception; none may escape into LLVM.
  try {
    SMTTranslator T(F, DL, &SE, Cfg);
    Analyzer A(F, DT, LI, *EnvOr, T);
    A.run();
    R.Groups = std::move(A.Groups);
    R.PerAccess = std::move(A.PerAccess);
    R.EnvError = std::move(A.EnvError);
  } catch (const z3::exception &Ex) {
    R.Groups.clear();
    R.PerAccess.clear();
    R.EnvError = std::string("Z3 exception during analysis: ") + Ex.msg();
  }
  return R;
}

SchedulePermission
SpatialVerificationResult::permissionFor(const Instruction *I) const {
  if (!EnvError.empty())
    return I->mayReadOrWriteMemory() ? SchedulePermission::Sequential
                                     : SchedulePermission::Parallel;
  auto It = PerAccess.find(I);
  return It == PerAccess.end() ? SchedulePermission::Parallel : It->second;
}

bool SpatialVerificationResult::allParallel() const {
  return EnvError.empty() && llvm::all_of(Groups, [](const GroupVerdict &G) {
           return G.Permission == SchedulePermission::Parallel;
         });
}

static char matrixCell(const PairEntry &E) {
  switch (E.Status) {
  case VCStatus::Proven:
    return '.';
  case VCStatus::Refuted:
    return 'X';
  case VCStatus::Unknown:
    return '?';
  case VCStatus::Timeout:
    return 'T';
  }
  return '?';
}

static void printMatrix(raw_ostream &OS, const GroupVerdict &G) {
  const size_t U = G.Claims.size();
  if (U < 2 || G.Matrix.empty())
    return;
  OS << "    VC matrix  (A_i & A_j => k_i != k_j):  '.' proven  'X' conflict"
        "  '?' unknown  'T' timeout\n";
  if (U <= 32) {
    std::vector<char> Grid(U * U, ' ');
    for (const PairEntry &E : G.Matrix)
      Grid[E.I * U + E.J] = matrixCell(E);
    OS << "         ";
    for (size_t J = 0; J < U; ++J)
      OS << right_justify(std::to_string(J), 3);
    OS << "\n";
    for (size_t I = 0; I + 1 < U; ++I) {
      OS << "      " << right_justify(std::to_string(I), 3);
      for (size_t J = 0; J < U; ++J)
        OS << "  " << (J == I ? '-' : Grid[I * U + J]);
      OS << "\n";
    }
  }
  unsigned Shown = 0;
  for (const PairEntry &E : G.Matrix) {
    if (E.Status == VCStatus::Proven || E.Witness.empty())
      continue;
    if (Shown++ == 8) {
      OS << "      ...\n";
      break;
    }
    OS << "      (" << E.I << "," << E.J << ") " << toString(E.Status) << ": "
       << E.Witness << "\n";
  }
}

void SpatialVerificationResult::print(raw_ostream &OS,
                                      const Function &F) const {
  OS << "HLS spatial verification: @" << F.getName() << "\n";
  if (!EnvError.empty()) {
    OS << "  environment error: " << EnvError
       << "\n  -> every memory access Sequential (conservative fallback)\n";
    return;
  }
  if (Groups.empty()) {
    OS << "  no memory accesses\n";
    return;
  }

  ModuleSlotTracker MST(F.getParent(), /*ShouldInitializeAllMetadata=*/false);
  MST.incorporateFunction(F);

  unsigned GI = 0;
  for (const GroupVerdict &G : Groups) {
    OS << "  group #" << GI++ << "  region: ";
    if (G.RegionHeader) {
      OS << "loop ";
      G.RegionHeader->printAsOperand(OS, /*PrintType=*/false, MST);
    } else {
      OS << "function top level";
    }
    OS << "\n    " << G.ArrayDesc << "\n";
    OS << "    cfu(E, A, N) = *_i bank(k_i, A, N) over " << G.Claims.size()
       << " claim(s):\n";
    for (size_t I = 0; I < G.Claims.size(); ++I) {
      const ClaimRecord &C = G.Claims[I];
      std::string Text;
      raw_string_ostream TS(Text);
      C.Access->print(TS, MST);
      OS << "      [" << I << "] " << StringRef(TS.str()).ltrim();
      if (C.Lane)
        OS << "   (lane " << C.Lane << ")";
      OS << "\n          expr = " << C.Offset << "    k = " << C.BankIndex
         << "\n";
    }
    OS << "    status: " << toString(G.Status)
       << " (paper: " << paperStatusName(G.Status) << ")  ->  "
       << toString(G.Permission) << "\n";
    if (!G.Reason.empty())
      OS << "    reason: " << G.Reason << "\n";
    printMatrix(OS, G);
    if (!G.SMTLib.empty())
      OS << "    query sent to Z3 (negated VC):\n" << G.SMTLib << "\n";
  }
}

PreservedAnalyses
SpatialVerificationPrinterPass::run(Function &F, FunctionAnalysisManager &FAM) {
  FAM.getResult<SpatialVerificationAnalysis>(F).print(OS, F);
  return PreservedAnalyses::all();
}

} // namespace hls_sv

//===----------------------------------------------------------------------===//
// Pass plugin registration
//===----------------------------------------------------------------------===//

static void addLatePrinter(ModulePassManager &MPM) {
  if (PrintAtEndOpt)
    MPM.addPass(createModuleToFunctionPassAdaptor(
        SpatialVerificationPrinterPass(errs())));
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "HLSSpatialVerification",
          LLVM_VERSION_STRING, [](PassBuilder &PB) {
            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass(
                      [] { return SpatialVerificationAnalysis(); });
                });

            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "print<hls-spatial-verify>") {
                    FPM.addPass(SpatialVerificationPrinterPass(errs()));
                    return true;
                  }
                  if (Name == "hls-spatial-verify") {
                    FPM.addPass(RequireAnalysisPass<SpatialVerificationAnalysis,
                                                    Function>());
                    return true;
                  }
                  if (Name == "hls-schedule-annotate") {
                    FPM.addPass(ScheduleAnnotatePass());
                    return true;
                  }
                  return false;
                });

    // "After loop unrolling" in a clang -O pipeline: the optimiser's
    // last extension point follows both full and runtime unrolling.
#if LLVM_VERSION_MAJOR >= 20
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel,
                   ThinOrFullLTOPhase) { addLatePrinter(MPM); });
#else
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  addLatePrinter(MPM);
                });
#endif
          }};
}

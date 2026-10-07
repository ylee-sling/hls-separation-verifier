//===- SMTTranslator.cpp - T(π ⊢ cfu) → Z3 verification condition --------===//
//
// HLS Spatial Verification Framework
//
//===----------------------------------------------------------------------===//

#include "hls-sv/SMTTranslator.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <unordered_set>

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

SchedulePermission permissionFor(VCStatus S) {
  // M(S) ≜ Parallel   if S = Sat
  //        Sequential if S ∈ {Unsat, Unknown, Tout}
  // Treating Unknown/Tout like Unsat is what keeps the framework sound in the
  // presence of non-linear arithmetic the oracle cannot settle.
  return S == VCStatus::Proven ? SchedulePermission::Parallel
                               : SchedulePermission::Sequential;
}

//===----------------------------------------------------------------------===//
// SymbolicEvaluator
//===----------------------------------------------------------------------===//

SymbolicEvaluator::SymbolicEvaluator(z3::context &Ctx, MonotonicStore &Store,
                                     const Function &F, const DataLayout &DL,
                                     ScalarEvolution *SE)
    : Ctx(Ctx), Store(Store), DL(DL), SE(SE),
      MST(F.getParent(), /*ShouldInitializeAllMetadata=*/false) {
  MST.incorporateFunction(F);
}

std::string SymbolicEvaluator::displayName(const Value *V) {
  std::string S;
  raw_string_ostream OS(S);
  V->printAsOperand(OS, /*PrintType=*/false, MST);
  return OS.str();
}

z3::expr SymbolicEvaluator::bv(const APInt &A) {
  const unsigned W = A.getBitWidth();
  if (W <= 64)
    return Ctx.bv_val(A.getZExtValue(), W);
  return Ctx.bv_val(llvm::toString(A, 10, /*Signed=*/false).c_str(), W);
}

z3::expr SymbolicEvaluator::freshSymbol(const Type *Ty, StringRef Hint) {
  std::string Name = (Hint + "!" + Twine(FreshCounter++)).str();
  if (Ty->isIntegerTy(1))
    return Ctx.bool_const(Name.c_str());
  return Ctx.bv_const(Name.c_str(), Ty->getIntegerBitWidth());
}

z3::expr SymbolicEvaluator::opaqueSymbol(const Value *V) {
  OpaqueRegs.insert(V);
  // One symbol per SSA register: the store memoises it, so the name is unique.
  std::string Name = displayName(V);
  if (V->getType()->isIntegerTy(1))
    return Ctx.bool_const(Name.c_str());
  z3::expr X = Ctx.bv_const(Name.c_str(), V->getType()->getIntegerBitWidth());
  addValueInvariants(V, X);
  return X;
}

void SymbolicEvaluator::addValueInvariants(const Value *V, const z3::expr &X) {
  const unsigned W = X.get_sort().bv_size();

  // Known bits, e.g. the low bits of an induction variable stepping by U
  // after unrolling. Without them `%i | 1` (an `or disjoint` copy) could be
  // confused with `%i` for odd %i, a false collision.
  KnownBits KB = computeKnownBits(V, DL);
  if (!KB.Zero.isZero())
    Invariants.push_back((X & bv(KB.Zero)) == bv(0, W));
  if (!KB.One.isZero())
    Invariants.push_back((X & bv(KB.One)) == bv(KB.One));

  // SCEV ranges (trip counts, !range metadata on data-dependent loads).
  if (SE && SE->isSCEVable(V->getType())) {
    const SCEV *S = SE->getSCEV(const_cast<Value *>(V));
    ConstantRange SR = SE->getSignedRange(S);
    if (!SR.isFullSet() && !SR.isEmptySet())
      Invariants.push_back(X >= bv(SR.getSignedMin()) &&
                           X <= bv(SR.getSignedMax()));
    ConstantRange UR = SE->getUnsignedRange(S);
    if (!UR.isFullSet() && !UR.isEmptySet())
      Invariants.push_back(z3::uge(X, bv(UR.getUnsignedMin())) &&
                           z3::ule(X, bv(UR.getUnsignedMax())));
  }
}

void SymbolicEvaluator::addDefinedness(const Instruction *I, z3::expr Fact) {
  Definedness[I].push_back(std::move(Fact));
}

void SymbolicEvaluator::recordPoisonFacts(const BinaryOperator *BO,
                                          const z3::expr &A, const z3::expr &B,
                                          const z3::expr &R) {
  const unsigned W = A.get_sort().bv_size();
  const unsigned Opc = BO->getOpcode();
  Z3_context C = Ctx;
  auto Mk = [&](Z3_ast Ast) {
    z3::expr E(Ctx, Ast);
    Ctx.check_error();
    return E;
  };

  if (const auto *OBO = dyn_cast<OverflowingBinaryOperator>(BO)) {
    const bool NSW = OBO->hasNoSignedWrap();
    const bool NUW = OBO->hasNoUnsignedWrap();
    switch (Opc) {
    case Instruction::Add:
      if (NSW) {
        addDefinedness(BO, Mk(Z3_mk_bvadd_no_overflow(C, A, B, true)));
        addDefinedness(BO, Mk(Z3_mk_bvadd_no_underflow(C, A, B)));
      }
      if (NUW)
        addDefinedness(BO, Mk(Z3_mk_bvadd_no_overflow(C, A, B, false)));
      break;
    case Instruction::Sub:
      if (NSW) {
        addDefinedness(BO, Mk(Z3_mk_bvsub_no_overflow(C, A, B)));
        addDefinedness(BO, Mk(Z3_mk_bvsub_no_underflow(C, A, B, true)));
      }
      if (NUW)
        addDefinedness(BO, Mk(Z3_mk_bvsub_no_underflow(C, A, B, false)));
      break;
    case Instruction::Mul:
      if (NSW) {
        addDefinedness(BO, Mk(Z3_mk_bvmul_no_overflow(C, A, B, true)));
        addDefinedness(BO, Mk(Z3_mk_bvmul_no_underflow(C, A, B)));
      }
      if (NUW)
        addDefinedness(BO, Mk(Z3_mk_bvmul_no_overflow(C, A, B, false)));
      break;
    case Instruction::Shl:
      if (NUW)
        addDefinedness(BO, z3::lshr(R, B) == A);
      if (NSW)
        addDefinedness(BO, z3::ashr(R, B) == A);
      break;
    default:
      break;
    }
  }

  if (const auto *PEO = dyn_cast<PossiblyExactOperator>(BO);
      PEO && PEO->isExact()) {
    switch (Opc) {
    case Instruction::UDiv:
      addDefinedness(BO, z3::urem(A, B) == bv(0, W));
      break;
    case Instruction::SDiv:
      addDefinedness(BO, z3::srem(A, B) == bv(0, W));
      break;
    case Instruction::LShr:
    case Instruction::AShr:
      addDefinedness(BO, z3::shl(R, B) == A);
      break;
    default:
      break;
    }
  }

#if LLVM_VERSION_MAJOR >= 18
  // `or disjoint` is what LoopUnroll emits for %i + c when %i is a multiple
  // of the unroll factor.
  if (const auto *PD = dyn_cast<PossiblyDisjointInst>(BO);
      PD && PD->isDisjoint())
    addDefinedness(BO, (A & B) == bv(0, W));
#endif

  switch (Opc) {
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    // Shifting by >= the bit width yields poison.
    addDefinedness(BO, z3::ult(B, bv(W, W)));
    break;
  case Instruction::UDiv:
  case Instruction::URem:
  case Instruction::SDiv:
  case Instruction::SRem:
    // Division by zero is immediate UB; the instruction dominates its uses,
    // so a non-zero divisor is necessary for any consumer to execute.
    addDefinedness(BO, B != bv(0, W));
    break;
  default:
    break;
  }
}

z3::expr SymbolicEvaluator::evaluate(const Value *V) {
  return evaluateImpl(V, 0);
}

z3::expr SymbolicEvaluator::asIndex(const Value *V, unsigned W) {
  z3::expr E = evaluate(V);
  if (E.is_bool()) // i1 index: sign-extended, so true ↦ −1
    return z3::ite(E, Ctx.bv_val(-1, W), bv(0, W));
  const unsigned SW = E.get_sort().bv_size();
  if (SW == W)
    return E;
  if (SW < W)
    return z3::sext(E, W - SW);
  return E.extract(W - 1, 0);
}

z3::expr SymbolicEvaluator::asBool(const Value *C) {
  if (!C->getType()->isIntegerTy(1))
    return freshSymbol(Type::getInt1Ty(C->getContext()), "cond");
  return evaluate(C);
}

z3::expr SymbolicEvaluator::evaluateImpl(const Value *V, unsigned Depth) {
  Type *Ty = V->getType();
  if (!Ty->isIntegerTy())
    llvm_unreachable("only integer SSA values have an arithmetic meaning");

  if (const auto *CI = dyn_cast<ConstantInt>(V))
    return Ty->isIntegerTy(1) ? Ctx.bool_val(CI->isOne()) : bv(CI->getValue());

  // undef, poison and constant expressions: unconstrained, and deliberately
  // not memoised (each use of undef may observe a different value).
  if (!MonotonicStore::isRegister(V))
    return freshSymbol(Ty, "const");

  // SSA: each register is evaluated once and then read back from s.
  if (std::optional<Val> S = Store.lookup(V))
    return S->term();

  z3::expr T(Ctx);
  bool Opaque = true;
  const auto *I = dyn_cast<Instruction>(V);
  if (I && Depth < MaxDepth && InProgress.insert(V).second) {
    T = interpret(I, Depth, Opaque);
    InProgress.erase(V);
    // A self-referential cone (only possible in unreachable code) may have
    // bound V while we were inside it; the first binding stands.
    if (std::optional<Val> S = Store.lookup(V))
      return S->term();
  }
  if (Opaque)
    T = opaqueSymbol(V);

  [[maybe_unused]] MonotonicStore::BindStatus St =
      Store.bind(V, Val::scalar(T));
  assert(St == MonotonicStore::BindStatus::Bound &&
         "SSA register evaluated twice");
  return T;
}

z3::expr SymbolicEvaluator::interpret(const Instruction *I, unsigned Depth,
                                      bool &Opaque) {
  Opaque = false;
  auto Op = [&](const Value *X) { return evaluateImpl(X, Depth + 1); };
  auto GiveUp = [&]() {
    Opaque = true;
    return z3::expr(Ctx);
  };

  // --- Binary arithmetic -------------------------------------------------
  if (const auto *BO = dyn_cast<BinaryOperator>(I)) {
    if (!BO->getType()->isIntegerTy())
      return GiveUp();
    z3::expr A = Op(BO->getOperand(0));
    z3::expr B = Op(BO->getOperand(1));
    if (BO->getType()->isIntegerTy(1)) {
      switch (BO->getOpcode()) {
      case Instruction::And:
        return A && B;
      case Instruction::Or:
        return A || B;
      case Instruction::Xor:
        return A != B;
      default:
        return GiveUp();
      }
    }
    z3::expr R(Ctx);
    switch (BO->getOpcode()) {
    case Instruction::Add:
      R = A + B;
      break;
    case Instruction::Sub:
      R = A - B;
      break;
    case Instruction::Mul:
      R = A * B;
      break; // non-affine allowed
    case Instruction::UDiv:
      R = z3::udiv(A, B);
      break;
    case Instruction::SDiv:
      R = A / B;
      break; // bvsdiv
    case Instruction::URem:
      R = z3::urem(A, B);
      break;
    case Instruction::SRem:
      R = z3::srem(A, B);
      break; // sign of dividend
    case Instruction::Shl:
      R = z3::shl(A, B);
      break;
    case Instruction::LShr:
      R = z3::lshr(A, B);
      break;
    case Instruction::AShr:
      R = z3::ashr(A, B);
      break;
    case Instruction::And:
      R = A & B;
      break;
    case Instruction::Or:
      R = A | B;
      break;
    case Instruction::Xor:
      R = A ^ B;
      break;
    default:
      return GiveUp();
    }
    recordPoisonFacts(BO, A, B, R);
    return R;
  }

  // --- Integer casts -----------------------------------------------------
  if (const auto *CI = dyn_cast<CastInst>(I)) {
    if (!CI->getSrcTy()->isIntegerTy() || !CI->getDestTy()->isIntegerTy())
      return GiveUp(); // ptrtoint, fp casts, vectors
    const unsigned SW = CI->getSrcTy()->getIntegerBitWidth();
    const unsigned DW = CI->getDestTy()->getIntegerBitWidth();
    z3::expr A = Op(CI->getOperand(0));
    switch (CI->getOpcode()) {
    case Instruction::ZExt:
      if (SW == 1)
        return z3::ite(A, bv(1, DW), bv(0, DW));
#if LLVM_VERSION_MAJOR >= 18
      if (CI->hasNonNeg())
        addDefinedness(CI, A >= bv(0, SW));
#endif
      return z3::zext(A, DW - SW);
    case Instruction::SExt:
      if (SW == 1)
        return z3::ite(A, Ctx.bv_val(-1, DW), bv(0, DW));
      return z3::sext(A, DW - SW);
    case Instruction::Trunc:
      if (DW == 1)
        return A.extract(0, 0) == bv(1, 1);
      return A.extract(DW - 1, 0);
    default:
      return GiveUp();
    }
  }

  // --- Comparisons -------------------------------------------------------
  if (const auto *Cmp = dyn_cast<ICmpInst>(I)) {
    if (!Cmp->getOperand(0)->getType()->isIntegerTy())
      return GiveUp(); // pointer / vector compares
    z3::expr A = Op(Cmp->getOperand(0));
    z3::expr B = Op(Cmp->getOperand(1));
    if (A.is_bool()) {
      if (Cmp->getPredicate() == CmpInst::ICMP_EQ)
        return A == B;
      if (Cmp->getPredicate() == CmpInst::ICMP_NE)
        return A != B;
      return GiveUp();
    }
    switch (Cmp->getPredicate()) {
    case CmpInst::ICMP_EQ:
      return A == B;
    case CmpInst::ICMP_NE:
      return A != B;
    case CmpInst::ICMP_UGT:
      return z3::ugt(A, B);
    case CmpInst::ICMP_UGE:
      return z3::uge(A, B);
    case CmpInst::ICMP_ULT:
      return z3::ult(A, B);
    case CmpInst::ICMP_ULE:
      return z3::ule(A, B);
    case CmpInst::ICMP_SGT:
      return A > B; // signed for bit-vectors
    case CmpInst::ICMP_SGE:
      return A >= B;
    case CmpInst::ICMP_SLT:
      return A < B;
    case CmpInst::ICMP_SLE:
      return A <= B;
    default:
      return GiveUp();
    }
  }

  // --- Select ------------------------------------------------------------
  if (const auto *Sel = dyn_cast<SelectInst>(I)) {
    if (!Sel->getCondition()->getType()->isIntegerTy(1))
      return GiveUp(); // vector select
    return z3::ite(Op(Sel->getCondition()), Op(Sel->getTrueValue()),
                   Op(Sel->getFalseValue()));
  }

  // --- min/max intrinsics (common in unrolled remainder bounds) ----------
  if (const auto *II = dyn_cast<IntrinsicInst>(I)) {
    if (!II->getType()->isIntegerTy() || II->getType()->isIntegerTy(1))
      return GiveUp();
    switch (II->getIntrinsicID()) {
    case Intrinsic::umin: {
      z3::expr A = Op(II->getArgOperand(0)), B = Op(II->getArgOperand(1));
      return z3::ite(z3::ule(A, B), A, B);
    }
    case Intrinsic::umax: {
      z3::expr A = Op(II->getArgOperand(0)), B = Op(II->getArgOperand(1));
      return z3::ite(z3::uge(A, B), A, B);
    }
    case Intrinsic::smin: {
      z3::expr A = Op(II->getArgOperand(0)), B = Op(II->getArgOperand(1));
      return z3::ite(A <= B, A, B);
    }
    case Intrinsic::smax: {
      z3::expr A = Op(II->getArgOperand(0)), B = Op(II->getArgOperand(1));
      return z3::ite(A >= B, A, B);
    }
    default:
      return GiveUp();
    }
  }

  // φ-nodes (loop-carried values), loads (data-dependent indices), calls and
  // freeze are havocked: an opaque symbol plus sound invariants.
  return GiveUp();
}

void SymbolicEvaluator::collectDefinednessFacts(
    const Value *Root, std::vector<z3::expr> &Out) const {
  SmallVector<const Value *, 16> Work{Root};
  SmallPtrSet<const Value *, 16> Seen;
  while (!Work.empty()) {
    const Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    const auto *I = dyn_cast<Instruction>(V);
    // Opaque symbols cut the cone: their operands are not part of the term.
    if (!I || OpaqueRegs.count(I) || !Store.contains(I))
      continue;
    auto It = Definedness.find(I);
    if (It != Definedness.end())
      Out.insert(Out.end(), It->second.begin(), It->second.end());
    for (const Use &U : I->operands())
      Work.push_back(U.get());
  }
}

//===----------------------------------------------------------------------===//
// SMTTranslator
//===----------------------------------------------------------------------===//

SMTTranslator::SMTTranslator(const Function &F, const DataLayout &DL,
                             ScalarEvolution *SE, TranslatorConfig Config)
    : Cfg(Config), Ctx(), Solver(Ctx, "QF_BV"), Store(),
      Eval(Ctx, Store, F, DL, SE) {
  configureSolver();
}

void SMTTranslator::configureSolver() {
  z3::params P(Ctx);
  // rlimit counts solver work in deterministic units: T_out is reproducible.
  P.set("rlimit", Cfg.RLimit);
  if (Cfg.WallClockMs)
    P.set("timeout", Cfg.WallClockMs);
  Solver.set(P);
}

void SMTTranslator::resetSolver() {
  // A fresh solver state per group keeps verdicts independent of the order
  // in which groups are processed.
  Solver.reset();
  configureSolver();
}

VCStatus SMTTranslator::classifyUnknown() {
  const std::string Why = Solver.reason_unknown();
  for (const char *Tag : {"resource", "rlimit", "timeout", "canceled"})
    if (Why.find(Tag) != std::string::npos)
      return VCStatus::Timeout;
  return VCStatus::Unknown;
}

VerificationCondition SMTTranslator::translate(const z3::expr &Pi,
                                               ArrayRef<BankClaim> K) {
  VerificationCondition VC{Pi, Ctx.bool_val(true), {}, {}};
  z3::expr_vector Body(Ctx);

  for (unsigned I = 0; I < K.size(); ++I) {
    for (unsigned J = I + 1; J < K.size(); ++J) {
      const BankClaim &CI = K[I];
      const BankClaim &CJ = K[J];
      PairEntry E;
      E.I = I;
      E.J = J;

      const z3::expr Both = (CI.Guard && CJ.Guard).simplify();
      if (CI.Pred.Array != CJ.Pred.Array || Both.is_false()) {
        // arr(A) * arr(B) for A ≠ B, or the two accesses never co-execute.
        E.Class = PairClass::Separated;
      } else {
        const z3::expr Same = (CI.Pred.Index == CJ.Pred.Index).simplify();
        if (Same.is_false())
          E.Class = PairClass::Separated;
        else if (Same.is_true())
          E.Class = PairClass::Collapse;
        else
          E.Class = PairClass::Symbolic;
      }

      switch (E.Class) {
      case PairClass::Separated:
        E.Status = VCStatus::Proven;
        E.Decided = true;
        break;
      case PairClass::Collapse:
        // bank(k) * bank(k): the same physical bank is claimed twice. With
        // unconditional guards this is P * P ⇒ ⊥ outright; otherwise the
        // oracle decides whether both accesses can actually co-execute.
        if (Both.is_true()) {
          E.Status = VCStatus::Refuted;
          E.Decided = true;
          E.Witness = "identical bank index: bank(k) * bank(k) => false";
        }
        break;
      case PairClass::Symbolic:
        break;
      }

      if (E.Class != PairClass::Separated) {
        z3::expr Ob =
            z3::implies(CI.Guard && CJ.Guard, CI.Pred.Index != CJ.Pred.Index);
        VC.Obligations.emplace_back(VC.Matrix.size(), Ob);
        Body.push_back(Ob);
      }
      VC.Matrix.push_back(std::move(E));
    }
  }

  // T(π ⊢ cfu(K, A, N)) ≜ π ⇒ ⋀_{i<j} ((A_i ∧ A_j) ⇒ k_i ≠ k_j)
  VC.Formula =
      z3::implies(Pi, Body.empty() ? Ctx.bool_val(true) : z3::mk_and(Body));
  return VC;
}

/// Uninterpreted constants (SSA symbols) occurring in E.
static void collectSymbols(const z3::expr &E, std::vector<z3::expr> &Out,
                           std::unordered_set<unsigned> &Seen) {
  if (!Seen.insert(Z3_get_ast_id(E.ctx(), E)).second || !E.is_app())
    return;
  if (E.is_const() && !E.is_numeral() &&
      E.decl().decl_kind() == Z3_OP_UNINTERPRETED) {
    Out.push_back(E);
    return;
  }
  for (unsigned I = 0; I < E.num_args(); ++I)
    collectSymbols(E.arg(I), Out, Seen);
}

std::string SMTTranslator::witness(const z3::model &M, const BankClaim &A,
                                   const BankClaim &B) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "both claim bank " << M.eval(A.Pred.Index, true).to_string()
     << " (offsets " << M.eval(A.Offset, true).to_string() << " and "
     << M.eval(B.Offset, true).to_string() << ")";

  std::vector<z3::expr> Syms;
  std::unordered_set<unsigned> Seen;
  collectSymbols(A.Offset, Syms, Seen);
  collectSymbols(B.Offset, Syms, Seen);
  if (!Syms.empty()) {
    OS << " when {";
    for (size_t I = 0; I < Syms.size() && I < 6; ++I)
      OS << (I ? ", " : "") << Syms[I].to_string() << " = "
         << M.eval(Syms[I], true).to_string();
    OS << (Syms.size() > 6 ? ", ...}" : "}");
  }
  return OS.str();
}

VCResult SMTTranslator::discharge(const VerificationCondition &VC,
                                  ArrayRef<BankClaim> K) {
  VCResult R;
  R.Matrix = VC.Matrix;

  if (K.size() > Cfg.MaxClaims) {
    R.Status = VCStatus::Timeout;
    R.Reason = "claim budget exceeded (" + std::to_string(K.size()) + " > " +
               std::to_string(Cfg.MaxClaims) + "): deterministic fallback";
    R.Permission = permissionFor(R.Status);
    return R;
  }

  try {
    bool AnyPending = false, AnyCollapse = false;
    for (const PairEntry &E : R.Matrix) {
      AnyPending |= !E.Decided;
      AnyCollapse |= E.Decided && E.Status == VCStatus::Refuted;
    }

    if (AnyCollapse) {
      // Spatial collapse found syntactically; no query needed for the verdict.
      R.Status = VCStatus::Refuted;
      for (const PairEntry &E : R.Matrix)
        if (E.Decided && E.Status == VCStatus::Refuted) {
          R.Reason = "spatial collapse between claims #" + std::to_string(E.I) +
                     " and #" + std::to_string(E.J) +
                     ": bank(k) * bank(k) => false";
          break;
        }
    } else if (!AnyPending) {
      R.Status = VCStatus::Proven;
      R.Reason = R.Matrix.empty() ? "single claim: cfu holds trivially"
                                  : "all claim pairs separated syntactically";
    } else {
      // Valid(VC) ⇔ Unsat(¬VC). ¬VC = π ∧ ⋁ (A_i ∧ A_j ∧ k_i = k_j).
      resetSolver();
      Solver.add(!VC.Formula);
      switch (Solver.check()) {
      case z3::unsat:
        R.Status = VCStatus::Proven;
        R.Reason = "negated VC is unsat: bank indices pairwise distinct "
                   "under pi";
        for (PairEntry &E : R.Matrix)
          if (!E.Decided) {
            E.Status = VCStatus::Proven;
            E.Decided = true;
          }
        break;
      case z3::sat: {
        R.Status = VCStatus::Refuted;
        z3::model M = Solver.get_model();
        // Locate an obligation falsified by the counterexample.
        for (const auto &[Idx, Ob] : VC.Obligations) {
          if (!M.eval(Ob, true).is_false())
            continue;
          PairEntry &E = R.Matrix[Idx];
          E.Status = VCStatus::Refuted;
          E.Decided = true;
          E.Witness = witness(M, K[E.I], K[E.J]);
          R.Reason = "counterexample for claims #" + std::to_string(E.I) +
                     " and #" + std::to_string(E.J) + ": " + E.Witness;
          break;
        }
        break;
      }
      case z3::unknown:
        R.Status = classifyUnknown();
        R.Reason = "solver inconclusive: " + Solver.reason_unknown();
        break;
      }
    }

    if (R.Status != VCStatus::Proven && Cfg.RefinePairs)
      refinePairs(VC, K, R);
  } catch (const z3::exception &Ex) {
    R.Status = VCStatus::Unknown;
    R.Reason = std::string("Z3 exception: ") + Ex.msg();
  }

  R.Permission = permissionFor(R.Status);
  return R;
}

void SMTTranslator::refinePairs(const VerificationCondition &VC,
                                ArrayRef<BankClaim> K, VCResult &R) {
  resetSolver();
  Solver.add(VC.Pi);
  for (const auto &[Idx, Ob] : VC.Obligations) {
    PairEntry &E = R.Matrix[Idx];
    if (E.Decided)
      continue;
    Solver.push();
    Solver.add(!Ob); // A_i ∧ A_j ∧ k_i = k_j
    switch (Solver.check()) {
    case z3::unsat:
      E.Status = VCStatus::Proven;
      break;
    case z3::sat:
      E.Status = VCStatus::Refuted;
      E.Witness = witness(Solver.get_model(), K[E.I], K[E.J]);
      break;
    case z3::unknown:
      E.Status = classifyUnknown();
      E.Witness = Solver.reason_unknown();
      break;
    }
    E.Decided = true;
    Solver.pop();
  }

  // Valid(⋀ obligations) ⇔ every obligation is valid, so the matrix can
  // upgrade an inconclusive conjoined query to Proven — never the reverse.
  bool AnyRefuted = false, AnyTimeout = false, AllProven = true;
  for (const PairEntry &E : R.Matrix) {
    if (E.Status == VCStatus::Proven)
      continue;
    AllProven = false;
    AnyRefuted |= E.Status == VCStatus::Refuted;
    AnyTimeout |= E.Status == VCStatus::Timeout;
  }
  if (AnyRefuted) {
    if (R.Status != VCStatus::Refuted) {
      R.Status = VCStatus::Refuted;
      R.Reason = "pairwise refinement found a bank conflict";
    }
  } else if (AllProven) {
    R.Status = VCStatus::Proven;
    R.Reason += "; pairwise refinement proved every obligation";
  } else {
    R.Status = AnyTimeout ? VCStatus::Timeout : VCStatus::Unknown;
  }
}

std::string SMTTranslator::toSMTLib(const VerificationCondition &VC) {
  z3::solver S(Ctx);
  S.add(!VC.Formula);
  return S.to_smt2();
}

} // namespace hls_sv

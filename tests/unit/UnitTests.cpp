//===- UnitTests.cpp - Unit tests for the core modules --------------------===//
//
// HLS Spatial Verification Framework
//
// Plain checks without a framework. `assert` is not used because it vanishes
// under NDEBUG (Release builds); CHECK always evaluates and reports.
//
//===----------------------------------------------------------------------===//

#include "hls-sv/BankPredicate.h"
#include "hls-sv/MemoryModel.h"
#include "hls-sv/SMTTranslator.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <z3++.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace hls_sv;

static unsigned Checks = 0;
static unsigned Failures = 0;

#define CHECK(Cond)                                                            \
  do {                                                                         \
    ++Checks;                                                                  \
    if (!(Cond)) {                                                             \
      ++Failures;                                                              \
      errs() << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #Cond "\n";  \
    }                                                                          \
  } while (false)

namespace {

/// A module with a few globals (array identifiers) and a function
/// `i64 @f(i64 %x, i64 %y)` to provide SSA registers.
struct Fixture {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  GlobalVariable *A = nullptr;
  GlobalVariable *B = nullptr;
  Function *F = nullptr;
  Argument *X = nullptr;
  Argument *Y = nullptr;

  Fixture() : M(std::make_unique<Module>("unit", Ctx)) {
    auto *ArrTy = ArrayType::get(Type::getInt32Ty(Ctx), 64);
    A = new GlobalVariable(*M, ArrTy, false, GlobalValue::ExternalLinkage,
                           ConstantAggregateZero::get(ArrTy), "A");
    B = new GlobalVariable(*M, ArrTy, false, GlobalValue::ExternalLinkage,
                           ConstantAggregateZero::get(ArrTy), "B");
    auto *I64 = Type::getInt64Ty(Ctx);
    auto *FTy = FunctionType::get(I64, {I64, I64}, false);
    F = Function::Create(FTy, GlobalValue::ExternalLinkage, "f", *M);
    X = F->getArg(0);
    X->setName("x");
    Y = F->getArg(1);
    Y->setName("y");
  }
};

ArrayInfo makeArray(const Value *ArrID, PartitionScheme P, uint64_t M,
                    uint64_t N, uint64_t Base = 0) {
  ArrayInfo A;
  A.ArrID = ArrID;
  A.Name = ArrID->getName().str();
  A.Base = Base;
  A.Capacity = M;
  A.Scheme = P;
  A.Factor = P == PartitionScheme::Complete ? M : N;
  A.ElemBytes = 4;
  A.Explicit = true;
  return A;
}

//===----------------------------------------------------------------------===//
// MemoryModel
//===----------------------------------------------------------------------===//

void testMonotonicStore() {
  Fixture Fx;
  z3::context Z;
  MonotonicStore S;
  const z3::expr X = Z.bv_const("x", 64);
  const z3::expr Y = Z.bv_const("y", 64);

  CHECK(S.bind(Fx.X, Val::scalar(X)) == MonotonicStore::BindStatus::Bound);
  CHECK(S.contains(Fx.X));
  CHECK(S.size() == 1);

  // Re-deriving the identical term is a memoised no-op.
  CHECK(S.bind(Fx.X, Val::scalar(X)) ==
        MonotonicStore::BindStatus::AlreadyBoundIdentical);

  // SSA: rebinding to a different term is rejected and s(x) is unchanged.
  CHECK(S.bind(Fx.X, Val::scalar(Y)) ==
        MonotonicStore::BindStatus::ImmutabilityViolation);
  CHECK(S.bind(Fx.X, Val::scalar(X + 1)) ==
        MonotonicStore::BindStatus::ImmutabilityViolation);
  std::optional<Val> Got = S.lookup(Fx.X);
  CHECK(Got && z3::eq(Got->term(), X));

  // Same term but a different kind (address vs scalar) is also a rebind.
  CHECK(S.bind(Fx.X, Loc::make(X, Fx.A)) ==
        MonotonicStore::BindStatus::ImmutabilityViolation);
  CHECK(S.size() == 1);

  // Constants are not registers and are never stored.
  Constant *C = ConstantInt::get(Type::getInt64Ty(Fx.Ctx), 7);
  CHECK(!MonotonicStore::isRegister(C));
  CHECK(S.bind(C, Val::scalar(Z.bv_val(7, 64))) ==
        MonotonicStore::BindStatus::NotARegister);
  CHECK(!S.contains(C));
  CHECK(!S.lookup(C));
  CHECK(S.size() == 1);

  // Globals and arguments are registers.
  CHECK(MonotonicStore::isRegister(Fx.A));
  CHECK(MonotonicStore::isRegister(Fx.Y));
  CHECK(!S.lookup(Fx.Y));
}

void testLocFromVal() {
  Fixture Fx;
  z3::context Z;

  // A scalar (loop counter, data word) is not a dereferenceable address.
  Val Scalar = Val::scalar(Z.bv_const("i", 64));
  CHECK(!Scalar.isAddress());
  CHECK(!Loc::fromVal(Scalar));
  CHECK(!Loc::fromVal(Val::scalar(Z.bv_val(0, 64))));
  CHECK(!Loc::fromVal(Val::scalar(Z.bool_const("c"))));

  // An address upcast to Val (L ⊂ Val) refines back into L.
  Loc L = Loc::make(Z.bv_val(12, 64), Fx.A);
  Val AsVal = L;
  CHECK(AsVal.isAddress());
  std::optional<Loc> Back = Loc::fromVal(AsVal);
  CHECK(Back.has_value());
  CHECK(Back && Back->region() == Fx.A);
  CHECK(Back && z3::eq(Back->term(), L.term()));
}

void testSpatialHeapStar() {
  Fixture Fx;
  z3::context Z;
  auto Cell = [&](const z3::expr &Addr, const Value *Region) {
    return SpatialHeap::pointsTo(Loc::make(Addr, Region),
                                 Val::scalar(Z.bv_const("v", 32)));
  };
  const z3::expr X = Z.bv_const("x", 64);
  const z3::expr Y = Z.bv_const("y", 64);

  // emp is the unit of *.
  StarResult E = SpatialHeap::star(SpatialHeap::emp(), Cell(X, Fx.A));
  CHECK(!E.collapsed() && E.Heap->size() == 1 && E.Obligations.empty());

  // Concrete: distinct numerals compose, equal numerals collapse (P*P => ⊥).
  StarResult R1 =
      SpatialHeap::star(Cell(Z.bv_val(3, 64), Fx.A), Cell(Z.bv_val(4, 64), Fx.A));
  CHECK(!R1.collapsed());
  CHECK(R1.Heap && R1.Heap->size() == 2);
  CHECK(R1.Obligations.empty());
  StarResult R2 =
      SpatialHeap::star(Cell(Z.bv_val(3, 64), Fx.A), Cell(Z.bv_val(3, 64), Fx.A));
  CHECK(R2.collapsed());

  // Syntactically identical symbolic address: MustAlias, collapse.
  StarResult R3 = SpatialHeap::star(Cell(X, Fx.A), Cell(X, Fx.A));
  CHECK(R3.collapsed());
  // x vs x + 1 is decided by the simplifier: disjoint, no obligation.
  StarResult R4 = SpatialHeap::star(Cell(X, Fx.A), Cell(X + 1, Fx.A));
  CHECK(!R4.collapsed() && R4.Obligations.empty());

  // Undetermined symbolic pair in one array: a pure obligation x != y.
  StarResult R5 = SpatialHeap::star(Cell(X, Fx.A), Cell(Y, Fx.A));
  CHECK(!R5.collapsed());
  CHECK(R5.Obligations.size() == 1);
  if (R5.Obligations.size() == 1) {
    z3::solver S(Z);
    S.add(!(R5.Obligations[0] == (X != Y)));
    CHECK(S.check() == z3::unsat); // the obligation is exactly x != y
  }
  // Symbolic vs concrete is also an obligation.
  StarResult R6 = SpatialHeap::star(Cell(Z.bv_val(5, 64), Fx.A), Cell(Y, Fx.A));
  CHECK(!R6.collapsed() && R6.Obligations.size() == 1);

  // Different arrays own disjoint footprints in Γ: no obligation needed.
  StarResult R7 = SpatialHeap::star(Cell(X, Fx.A), Cell(Y, Fx.B));
  CHECK(!R7.collapsed() && R7.Obligations.empty());

  // A collapse anywhere in a larger heap collapses the composition.
  StarResult Big = SpatialHeap::star(Cell(Z.bv_val(1, 64), Fx.A),
                                     Cell(Z.bv_val(2, 64), Fx.A));
  CHECK(!Big.collapsed());
  if (!Big.collapsed()) {
    StarResult R8 = SpatialHeap::star(*Big.Heap, Cell(Z.bv_val(2, 64), Fx.A));
    CHECK(R8.collapsed());
  }
}

//===----------------------------------------------------------------------===//
// BankPredicate
//===----------------------------------------------------------------------===//

void testCompositionRuleGrid() {
  Fixture Fx;
  z3::context Z;
  unsigned Cases = 0;
  auto Expect = [&](const ArrayInfo &A) {
    ++Cases;
    Error E = verifyCompositionRule(Z, A);
    if (E) {
      errs() << "  composition rule failed for M=" << *A.Capacity
             << " N=" << A.Factor << " " << toString(A.Scheme) << ": "
             << toString(std::move(E)) << "\n";
      CHECK(false);
    }
  };

  // Includes M=10, N=4 (ragged last block / bank) and N > M (empty banks).
  for (uint64_t M : {1, 2, 3, 4, 7, 8, 10, 16, 33, 64}) {
    for (uint64_t N = 1; N <= M + 3; ++N) {
      Expect(makeArray(Fx.A, PartitionScheme::Cyclic, M, N));
      Expect(makeArray(Fx.A, PartitionScheme::Block, M, N, /*Base=*/1000));
    }
    Expect(makeArray(Fx.A, PartitionScheme::Complete, M, M));
  }
  CHECK(Cases > 300);

  // Spot-check the bank shapes for M=10, N=4.
  ArrayInfo Cyc = makeArray(Fx.A, PartitionScheme::Cyclic, 10, 4);
  CHECK(materializeBank(Z, Cyc, 0).size() == 3); // 0, 4, 8
  CHECK(materializeBank(Z, Cyc, 1).size() == 3); // 1, 5, 9
  CHECK(materializeBank(Z, Cyc, 2).size() == 2); // 2, 6
  CHECK(materializeBank(Z, Cyc, 3).size() == 2); // 3, 7
  ArrayInfo Blk = makeArray(Fx.A, PartitionScheme::Block, 10, 4);
  CHECK(Blk.blockSize() == 3);
  CHECK(materializeBank(Z, Blk, 0).size() == 3); // 0..2
  CHECK(materializeBank(Z, Blk, 2).size() == 3); // 6..8
  CHECK(materializeBank(Z, Blk, 3).size() == 1); // 9
  // N > M: the surplus banks unfold to emp.
  ArrayInfo Wide = makeArray(Fx.A, PartitionScheme::Cyclic, 3, 5);
  CHECK(materializeBank(Z, Wide, 4).isEmp());
  ArrayInfo WideBlk = makeArray(Fx.A, PartitionScheme::Block, 3, 5);
  CHECK(WideBlk.blockSize() == 1);
  CHECK(materializeBank(Z, WideBlk, 4).isEmp());

  // Unknown M is skipped, not rejected.
  ArrayInfo Unknown = makeArray(Fx.A, PartitionScheme::Cyclic, 8, 4);
  Unknown.Capacity.reset();
  CHECK(!errorToBool(verifyCompositionRule(Z, Unknown)));
}

//===----------------------------------------------------------------------===//
// SMTTranslator
//===----------------------------------------------------------------------===//

struct DischargeOutcome {
  VCStatus Status;
  SchedulePermission Permission;
  std::string Reason;
  std::vector<PairEntry> Matrix;
};

/// Two unconditional claims on @A (cyclic N, unknown M) at offsets
/// x + C0 and x + C1, with π = true.
DischargeOutcome dischargePair(uint64_t N, uint64_t C0, uint64_t C1) {
  Fixture Fx;
  SMTTranslator T(*Fx.F, Fx.M->getDataLayout(), /*SE=*/nullptr);
  z3::context &Z = T.context();
  ArrayInfo A = makeArray(Fx.A, PartitionScheme::Cyclic, 64, N);
  A.Capacity.reset(); // no in-bounds facts: wrap-around is fully exposed

  const z3::expr X = T.evaluator().evaluate(Fx.X);
  std::vector<BankClaim> K;
  for (uint64_t C : {C0, C1}) {
    z3::expr Off = (X + Z.bv_val(C, 64)).simplify();
    z3::expr Idx = bankIndex(A, Off).simplify();
    K.push_back(BankClaim{BankPredicate{&A, Idx}, Off, Z.bool_val(true),
                          nullptr, 0});
  }
  VerificationCondition VC = T.translate(Z.bool_val(true), K);
  VCResult R = T.discharge(VC, K);
  return {R.Status, R.Permission, R.Reason, R.Matrix};
}

void testTranslateDischarge() {
  // x mod 4 vs (x+1) mod 4: distinct for every 64-bit x (incl. wrap-around).
  // (Z3's simplifier already decides both mod-4 pairs: the low two bits of
  // x and x+1 differ syntactically, and x+4 has the same low bits as x. Only
  // the verdicts are checked here; the mod-3 cases below reach the solver.)
  DischargeOutcome P = dischargePair(4, 0, 1);
  CHECK(P.Status == VCStatus::Proven);
  CHECK(P.Permission == SchedulePermission::Parallel);
  CHECK(P.Matrix.size() == 1);
  CHECK(!P.Matrix.empty() && P.Matrix[0].Status == VCStatus::Proven);

  // x mod 4 vs (x+4) mod 4: always the same bank.
  DischargeOutcome R = dischargePair(4, 0, 4);
  CHECK(R.Status == VCStatus::Refuted);
  CHECK(R.Permission == SchedulePermission::Sequential);
  CHECK(!R.Matrix.empty() && R.Matrix[0].Status == VCStatus::Refuted);
  CHECK(!R.Matrix.empty() && !R.Matrix[0].Witness.empty());

  // x mod 3 vs (x+1) mod 3 needs the oracle, and is NOT valid in QF_BV:
  // x = 2^64-1 wraps to 0, and both are 0 mod 3 (invariant 3). A sat model of
  // ¬VC must be reported as Refuted (never as "sat => safe"), with the
  // wrap-around witness.
  DischargeOutcome W = dischargePair(3, 0, 1);
  CHECK(!W.Matrix.empty() && W.Matrix[0].Class == PairClass::Symbolic);
  CHECK(W.Status == VCStatus::Refuted);
  CHECK(W.Permission == SchedulePermission::Sequential);
  CHECK(W.Reason.find("counterexample") != std::string::npos);
  CHECK(!W.Matrix.empty() &&
        W.Matrix[0].Witness.find("%x = #xffffffffffffffff") !=
            std::string::npos);

  // x mod 3 vs (x+3) mod 3 conflicts at x = 0 already.
  DischargeOutcome W3 = dischargePair(3, 0, 3);
  CHECK(!W3.Matrix.empty() && W3.Matrix[0].Class == PairClass::Symbolic);
  CHECK(W3.Status == VCStatus::Refuted);

  // Single claim: cfu holds trivially.
  {
    Fixture Fx;
    SMTTranslator T(*Fx.F, Fx.M->getDataLayout(), nullptr);
    z3::context &Z = T.context();
    ArrayInfo A = makeArray(Fx.A, PartitionScheme::Cyclic, 64, 4);
    z3::expr X = T.evaluator().evaluate(Fx.X);
    std::vector<BankClaim> K{BankClaim{BankPredicate{&A, bankIndex(A, X)}, X,
                                       Z.bool_val(true), nullptr, 0}};
    VCResult Res = T.discharge(T.translate(Z.bool_val(true), K), K);
    CHECK(Res.Status == VCStatus::Proven);
  }

  // Contradictory guards separate a same-bank pair without a query.
  {
    Fixture Fx;
    SMTTranslator T(*Fx.F, Fx.M->getDataLayout(), nullptr);
    z3::context &Z = T.context();
    ArrayInfo A = makeArray(Fx.A, PartitionScheme::Cyclic, 64, 4);
    z3::expr X = T.evaluator().evaluate(Fx.X);
    z3::expr C = Z.bool_const("c");
    z3::expr Idx = bankIndex(A, X);
    std::vector<BankClaim> K{
        BankClaim{BankPredicate{&A, Idx}, X, C, nullptr, 0},
        BankClaim{BankPredicate{&A, Idx}, X, !C, nullptr, 0}};
    VerificationCondition VC = T.translate(Z.bool_val(true), K);
    CHECK(VC.Matrix.size() == 1 &&
          VC.Matrix[0].Class == PairClass::Separated);
    CHECK(T.discharge(VC, K).Status == VCStatus::Proven);
  }
}

void testPermissionMapping() {
  // M(S): only a proven VC (paper: Sat) is Parallel.
  CHECK(permissionFor(VCStatus::Proven) == SchedulePermission::Parallel);
  CHECK(permissionFor(VCStatus::Refuted) == SchedulePermission::Sequential);
  CHECK(permissionFor(VCStatus::Unknown) == SchedulePermission::Sequential);
  CHECK(permissionFor(VCStatus::Timeout) == SchedulePermission::Sequential);

  CHECK(paperStatusName(VCStatus::Proven) == "Sat");
  CHECK(paperStatusName(VCStatus::Refuted) == "Unsat");
  CHECK(paperStatusName(VCStatus::Unknown) == "Unknown");
  CHECK(paperStatusName(VCStatus::Timeout) == "Tout");
}

} // namespace

int main() {
  struct {
    const char *Name;
    void (*Fn)();
  } Tests[] = {
      {"MonotonicStore immutability", testMonotonicStore},
      {"Loc::fromVal refinement", testLocFromVal},
      {"SpatialHeap::star", testSpatialHeapStar},
      {"verifyCompositionRule grid", testCompositionRuleGrid},
      {"translate/discharge", testTranslateDischarge},
      {"permissionFor (M(S))", testPermissionMapping},
  };

  for (const auto &T : Tests) {
    const unsigned Before = Failures;
    try {
      T.Fn();
    } catch (const z3::exception &Ex) {
      ++Failures;
      errs() << "  unexpected Z3 exception: " << Ex.msg() << "\n";
    }
    outs() << (Failures == Before ? "[PASS] " : "[FAIL] ") << T.Name << "\n";
  }
  outs() << Checks << " checks, " << Failures << " failure(s)\n";
  return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

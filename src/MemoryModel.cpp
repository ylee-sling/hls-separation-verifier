//===- MemoryModel.cpp - Dual-state Separation Logic memory model ---------===//
//
// HLS Spatial Verification Framework
//
//===----------------------------------------------------------------------===//

#include "hls-sv/MemoryModel.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include <cassert>

using namespace llvm;

namespace hls_sv {

//===----------------------------------------------------------------------===//
// Val / Loc
//===----------------------------------------------------------------------===//

void Val::print(raw_ostream &OS) const {
  OS << (isAddress() ? "loc(" : "val(") << term().to_string();
  if (Region)
    OS << " in @" << (Region->hasName() ? Region->getName() : "<anon>");
  OS << ")";
}

Loc Loc::make(z3::expr Address, const llvm::Value *Region) {
  assert(Address.is_bv() && "physical addresses are bit-vectors");
  return Loc(std::move(Address), Region);
}

std::optional<Loc> Loc::fromVal(const Val &V) {
  // L ⊂ Val is strict: only values produced as addresses refine into L.
  if (!V.isAddress())
    return std::nullopt;
  return Loc(V.term(), V.region());
}

//===----------------------------------------------------------------------===//
// MonotonicStore
//===----------------------------------------------------------------------===//

bool MonotonicStore::isRegister(const Value *V) {
  return isa<Instruction, Argument, GlobalValue>(V);
}

MonotonicStore::BindStatus MonotonicStore::bind(const Value *Reg,
                                                const Val &V) {
  if (!isRegister(Reg))
    return BindStatus::NotARegister;

  auto [It, Inserted] = Map.try_emplace(Reg, V);
  if (Inserted)
    return BindStatus::Bound;

  // SSA: a register is defined exactly once. Re-deriving the identical term
  // (memoised evaluation) is benign; anything else would be an s → s' update.
  const Val &Old = It->second;
  if (Old.kind() == V.kind() && z3::eq(Old.term(), V.term()))
    return BindStatus::AlreadyBoundIdentical;
  return BindStatus::ImmutabilityViolation;
}

std::optional<Val> MonotonicStore::lookup(const Value *Reg) const {
  auto It = Map.find(Reg);
  if (It == Map.end())
    return std::nullopt;
  return It->second;
}

//===----------------------------------------------------------------------===//
// SpatialHeap
//===----------------------------------------------------------------------===//

static std::optional<uint64_t> asNumeral(const z3::expr &E) {
  uint64_t N = 0;
  if (E.is_numeral() && E.is_numeral_u64(N))
    return N;
  return std::nullopt;
}

AliasVerdict classifyLocs(const Loc &A, const Loc &B) {
  // Distinct arrays are distinct SRAM macros with disjoint ranges in Γ, so
  // their cells are separated without consulting the oracle.
  if (A.region() && B.region() && A.region() != B.region())
    return AliasVerdict::Disjoint;

  // Fast path for concrete addresses.
  if (auto NA = asNumeral(A.term()))
    if (auto NB = asNumeral(B.term()))
      return *NA == *NB ? AliasVerdict::MustAlias : AliasVerdict::Disjoint;

  assert(z3::eq(A.term().get_sort(), B.term().get_sort()) &&
         "addresses must share one bit-vector width");
  z3::expr Eq = (A.term() == B.term()).simplify();
  if (Eq.is_true())
    return AliasVerdict::MustAlias; // syntactically the same location
  if (Eq.is_false())
    return AliasVerdict::Disjoint;
  return AliasVerdict::Undetermined;
}

SpatialHeap SpatialHeap::pointsTo(Loc L, Val V) {
  SpatialHeap H;
  H.append(PointsTo{std::move(L), std::move(V)});
  return H;
}

void SpatialHeap::append(const PointsTo &C) {
  size_t Idx = Cells.size();
  Cells.push_back(C);
  if (auto N = asNumeral(C.Address.term()))
    ConcreteIndex.emplace(*N, Idx);
  else
    SymbolicCells.push_back(Idx);
}

StarResult SpatialHeap::star(const SpatialHeap &H1, const SpatialHeap &H2) {
  StarResult R;
  SpatialHeap Out = H1;

  auto Collapse = [] { return StarResult{std::nullopt, {}}; };

  for (const PointsTo &C2 : H2.Cells) {
    std::optional<uint64_t> N2 = asNumeral(C2.Address.term());

    // Concrete vs concrete: one hash lookup. A hit means
    // (ℓ ↦ v) * (ℓ ↦ v') — the same resource owned twice: P * P ⇒ ⊥.
    if (N2 && H1.ConcreteIndex.count(*N2))
      return Collapse();

    // Everything that involves a symbolic address goes through the full
    // classifier; undetermined pairs become pure obligations ℓ1 ≠ ℓ2.
    auto Compare = [&](const PointsTo &C1) -> bool {
      switch (classifyLocs(C1.Address, C2.Address)) {
      case AliasVerdict::Disjoint:
        return true;
      case AliasVerdict::MustAlias:
        return false;
      case AliasVerdict::Undetermined:
        R.Obligations.push_back(C1.Address.term() != C2.Address.term());
        return true;
      }
      return false;
    };

    if (N2) {
      for (size_t I : H1.SymbolicCells)
        if (!Compare(H1.Cells[I]))
          return Collapse();
    } else {
      for (const PointsTo &C1 : H1.Cells)
        if (!Compare(C1))
          return Collapse();
    }
    Out.append(C2);
  }

  R.Heap = std::move(Out);
  return R;
}

std::optional<Val> SpatialHeap::lookup(const Loc &L) const {
  if (auto N = asNumeral(L.term())) {
    auto It = ConcreteIndex.find(*N);
    if (It != ConcreteIndex.end())
      return Cells[It->second].Contents;
  }
  for (const PointsTo &C : Cells)
    if (classifyLocs(C.Address, L) == AliasVerdict::MustAlias)
      return C.Contents;
  return std::nullopt;
}

void SpatialHeap::print(raw_ostream &OS) const {
  if (Cells.empty()) {
    OS << "emp";
    return;
  }
  bool First = true;
  for (const PointsTo &C : Cells) {
    if (!First)
      OS << " * ";
    First = false;
    OS << C.Address.term().to_string() << " |-> "
       << C.Contents.term().to_string();
  }
}

} // namespace hls_sv

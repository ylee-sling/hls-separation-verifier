//===- BankPredicate.cpp - Γ(A) and polymorphic bank predicates -----------===//
//
// HLS Spatial Verification Framework
//
//===----------------------------------------------------------------------===//

#include "hls-sv/BankPredicate.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <map>

using namespace llvm;

namespace hls_sv {

/// Footprint reserved in L for an array whose capacity M is unknown (an
/// unannotated pointer argument), keeping footprints of distinct arrays
/// disjoint.
static constexpr uint64_t kUnknownExtent = uint64_t(1) << 32;
/// Width of the physical address space used by materialised heaps.
static constexpr unsigned kAddrWidth = 64;

static Error gammaError(const Twine &Msg) {
  return make_error<StringError>("Gamma: " + Msg, inconvertibleErrorCode());
}

static std::string nameOf(const Value *V) {
  return V->hasName() ? V->getName().str() : std::string("<anon>");
}

//===----------------------------------------------------------------------===//
// Schemes and specs
//===----------------------------------------------------------------------===//

StringRef toString(PartitionScheme P) {
  switch (P) {
  case PartitionScheme::Cyclic:
    return "cyclic";
  case PartitionScheme::Block:
    return "block";
  case PartitionScheme::Complete:
    return "complete";
  }
  llvm_unreachable("unknown partition scheme");
}

std::optional<PartitionScheme> parsePartitionScheme(StringRef S) {
  if (S.equals_insensitive("cyclic"))
    return PartitionScheme::Cyclic;
  if (S.equals_insensitive("block"))
    return PartitionScheme::Block;
  if (S.equals_insensitive("complete"))
    return PartitionScheme::Complete;
  return std::nullopt;
}

Expected<PartitionSpec> parsePartitionSpec(const MDNode *MD) {
  if (!MD)
    return gammaError("null partition metadata");
  if (MD->getNumOperands() % 2 != 0)
    return gammaError("partition metadata must be key/value pairs");

  PartitionSpec S;
  for (unsigned I = 0; I < MD->getNumOperands(); I += 2) {
    const auto *Key = dyn_cast_or_null<MDString>(MD->getOperand(I).get());
    if (!Key)
      return gammaError("expected an MDString key at operand " + Twine(I));
    StringRef K = Key->getString();
    const MDOperand &V = MD->getOperand(I + 1);

    if (K == "scheme") {
      const auto *Str = dyn_cast_or_null<MDString>(V.get());
      if (!Str)
        return gammaError("'scheme' expects a string value");
      S.Scheme = parsePartitionScheme(Str->getString());
      if (!S.Scheme)
        return gammaError("unknown partition scheme '" + Str->getString() +
                          "'");
      continue;
    }

    const auto *CI = mdconst::dyn_extract_or_null<ConstantInt>(V);
    if (!CI)
      return gammaError("key '" + K + "' expects an integer constant");
    const uint64_t N = CI->getZExtValue();
    if (K == "factor")
      S.Factor = N;
    else if (K == "capacity")
      S.Capacity = N;
    else if (K == "elem_bytes")
      S.ElemBytes = N;
    else if (K == "base")
      S.Base = N;
    else if (K == "arg")
      S.ArgNo = static_cast<unsigned>(N);
    else
      return gammaError("unknown key '" + K + "'");
  }
  return S;
}

Expected<GammaOverride> parseGammaOverride(StringRef Text) {
  SmallVector<StringRef, 5> Parts;
  Text.split(Parts, ':');
  if (Parts.size() < 3 || Parts.size() > 5)
    return gammaError("override '" + Text +
                      "' must be <name>:<scheme>:<N>[:<M>[:<elem_bytes>]]");
  GammaOverride O;
  O.Name = Parts[0].str();
  O.Spec.Scheme = parsePartitionScheme(Parts[1]);
  if (!O.Spec.Scheme)
    return gammaError("unknown partition scheme '" + Parts[1] + "'");
  auto ParseNum = [&](StringRef P, std::optional<uint64_t> &Out) -> Error {
    uint64_t N = 0;
    if (P.getAsInteger(10, N))
      return gammaError("'" + P + "' is not a number in override '" + Text +
                        "'");
    Out = N;
    return Error::success();
  };
  if (Error E = ParseNum(Parts[2], O.Spec.Factor))
    return std::move(E);
  if (Parts.size() > 3)
    if (Error E = ParseNum(Parts[3], O.Spec.Capacity))
      return std::move(E);
  if (Parts.size() > 4)
    if (Error E = ParseNum(Parts[4], O.Spec.ElemBytes))
      return std::move(E);
  return O;
}

//===----------------------------------------------------------------------===//
// ArrayInfo
//===----------------------------------------------------------------------===//

uint64_t ArrayInfo::blockSize() const {
  assert(Capacity && Factor > 0 && "Block partitioning needs M and N");
  return (*Capacity + Factor - 1) / Factor; // B = ⌈M/N⌉
}

void ArrayInfo::print(raw_ostream &OS) const {
  OS << "@" << Name << ": Gamma = (alpha=" << Base << ", M=";
  if (Capacity)
    OS << *Capacity;
  else
    OS << "?";
  OS << ", " << toString(Scheme) << "), N=" << Factor << ", elem=" << ElemBytes
     << "B";
  if (Scheme == PartitionScheme::Block && Capacity)
    OS << ", B=" << blockSize();
  OS << (Explicit ? "" : " [implicit monolithic SRAM]");
}

//===----------------------------------------------------------------------===//
// StaticHardwareEnv
//===----------------------------------------------------------------------===//

namespace {
/// What the IR type of an allocation tells us. Nested array types are only
/// peeled to obtain M and sizeof(element); the index arithmetic itself is
/// always taken from getelementptr, never re-derived from C/C++ shapes.
struct Shape {
  std::optional<uint64_t> Count;
  std::optional<uint64_t> ElemBytes;
};
} // namespace

static Shape inferShape(const Value *ArrID, const DataLayout &DL) {
  Type *Ty = nullptr;
  std::optional<uint64_t> Mul = 1;
  if (const auto *GV = dyn_cast<GlobalVariable>(ArrID)) {
    Ty = GV->getValueType();
  } else if (const auto *AI = dyn_cast<AllocaInst>(ArrID)) {
    Ty = AI->getAllocatedType();
    if (AI->isArrayAllocation()) {
      if (const auto *C = dyn_cast<ConstantInt>(AI->getArraySize()))
        Mul = C->getZExtValue();
      else
        Mul = std::nullopt; // dynamically sized alloca
    }
  } else {
    return {}; // pointer argument: opaque `ptr` carries no shape
  }

  uint64_t Count = 1;
  while (auto *AT = dyn_cast<ArrayType>(Ty)) {
    Count *= AT->getNumElements();
    Ty = AT->getElementType();
  }
  if (!Ty->isSized())
    return {};
  TypeSize TS = DL.getTypeAllocSize(Ty);
  if (TS.isScalable() || TS.getFixedValue() == 0)
    return {};

  Shape S;
  S.ElemBytes = TS.getFixedValue();
  if (Mul)
    S.Count = Count * *Mul;
  return S;
}

const ArrayInfo *StaticHardwareEnv::lookup(const Value *ArrID) const {
  auto It = Index.find(ArrID);
  return It == Index.end() ? nullptr : It->second;
}

Error StaticHardwareEnv::assignBase(ArrayInfo &A,
                                    std::optional<uint64_t> Requested) {
  const uint64_t Extent = A.Capacity.value_or(kUnknownExtent);
  const uint64_t Base = Requested.value_or(NextBase);
  // Distinct ArrIDs must own disjoint footprints in L; otherwise two
  // identifiers could alias the same physical cells and the per-array
  // separation used by the heap would be unsound.
  for (const auto &E : Entries) {
    const uint64_t EExt = E->Capacity.value_or(kUnknownExtent);
    if (Base < E->Base + EExt && E->Base < Base + Extent)
      return gammaError("physical range of @" + A.Name + " overlaps @" +
                        E->Name);
  }
  A.Base = Base;
  NextBase = std::max(NextBase, Base + Extent);
  return Error::success();
}

Error StaticHardwareEnv::add(const Value *ArrID, const PartitionSpec &Spec,
                             const DataLayout &DL) {
  if (Index.count(ArrID))
    return gammaError("duplicate partition directive for @" + nameOf(ArrID));
  if (!Spec.Scheme)
    return gammaError("missing 'scheme' for @" + nameOf(ArrID));

  auto A = std::make_unique<ArrayInfo>();
  A->ArrID = ArrID;
  A->Name = nameOf(ArrID);
  A->Explicit = true;
  A->Scheme = *Spec.Scheme;

  const Shape Sh = inferShape(ArrID, DL);
  if (Spec.Capacity && Sh.Count && *Spec.Capacity != *Sh.Count)
    // A smaller M would make the in-bounds facts used in π unsound.
    return gammaError("capacity " + Twine(*Spec.Capacity) + " of @" + A->Name +
                      " disagrees with its IR allocation of " +
                      Twine(*Sh.Count) + " elements");
  A->Capacity = Spec.Capacity ? Spec.Capacity : Sh.Count;
  if (A->Capacity && *A->Capacity == 0)
    return gammaError("capacity of @" + A->Name + " must be positive");

  const std::optional<uint64_t> EB =
      Spec.ElemBytes ? Spec.ElemBytes : Sh.ElemBytes;
  if (!EB || *EB == 0)
    return gammaError("element size of @" + A->Name +
                      " is unknown; add 'elem_bytes'");
  A->ElemBytes = *EB;

  switch (A->Scheme) {
  case PartitionScheme::Cyclic:
  case PartitionScheme::Block:
    if (!Spec.Factor || *Spec.Factor == 0)
      return gammaError("'factor' (N >= 1) required for @" + A->Name);
    A->Factor = *Spec.Factor;
    if (A->Scheme == PartitionScheme::Block && !A->Capacity)
      return gammaError("block partitioning of @" + A->Name +
                        " needs a known capacity M");
    break;
  case PartitionScheme::Complete:
    if (!A->Capacity)
      return gammaError("complete partitioning of @" + A->Name +
                        " needs a known capacity M");
    A->Factor = *A->Capacity; // boundary case N = M
    break;
  }

  if (Error E = assignBase(*A, Spec.Base))
    return E;
  Index[ArrID] = A.get();
  Entries.push_back(std::move(A));
  return Error::success();
}

const ArrayInfo *StaticHardwareEnv::lookupOrImplicit(const Value *ArrID,
                                                     const DataLayout &DL) {
  if (const ArrayInfo *A = lookup(ArrID))
    return A;
  if (!isa<GlobalVariable, AllocaInst, Argument>(ArrID))
    return nullptr;
  if (const auto *Arg = dyn_cast<Argument>(ArrID))
    if (!Arg->getType()->isPointerTy())
      return nullptr;

  // No directive: a monolithic single-port SRAM, i.e. bank(0, A, 1) is the
  // whole array and every bank index evaluates to 0.
  auto A = std::make_unique<ArrayInfo>();
  A->ArrID = ArrID;
  A->Name = nameOf(ArrID);
  A->Explicit = false;
  A->Scheme = PartitionScheme::Cyclic;
  A->Factor = 1;
  const Shape Sh = inferShape(ArrID, DL);
  A->Capacity = Sh.Count;
  A->ElemBytes = Sh.ElemBytes.value_or(1);
  if (Error E = assignBase(*A, std::nullopt)) {
    consumeError(std::move(E)); // cannot happen for fresh bases
    return nullptr;
  }
  ArrayInfo *Raw = A.get();
  Index[ArrID] = Raw;
  Entries.push_back(std::move(A));
  return Raw;
}

Expected<StaticHardwareEnv>
StaticHardwareEnv::build(const Function &F, ArrayRef<GammaOverride> Overrides) {
  StaticHardwareEnv Env;
  const Module &M = *F.getParent();
  const DataLayout &DL = M.getDataLayout();

  auto FindOverride = [&](const Value *V) -> const PartitionSpec * {
    if (!V->hasName())
      return nullptr;
    for (const GammaOverride &O : Overrides)
      if (O.Name == V->getName())
        return &O.Spec;
    return nullptr;
  };

  // Command-line overrides take precedence over IR metadata.
  auto AddFrom = [&](const Value *V, const MDNode *MD) -> Error {
    if (const PartitionSpec *Ov = FindOverride(V))
      return Env.add(V, *Ov, DL);
    if (!MD)
      return Error::success();
    Expected<PartitionSpec> S = parsePartitionSpec(MD);
    if (!S)
      return S.takeError();
    return Env.add(V, *S, DL);
  };

  for (const GlobalVariable &GV : M.globals())
    if (Error E = AddFrom(&GV, GV.getMetadata(kPartitionMDName)))
      return std::move(E);

  for (const Instruction &I : instructions(F))
    if (const auto *AI = dyn_cast<AllocaInst>(&I))
      if (Error E = AddFrom(AI, AI->getMetadata(kPartitionMDName)))
        return std::move(E);

  std::map<unsigned, PartitionSpec> ArgSpecs;
  if (const MDNode *List = F.getMetadata(kPartitionArgsMDName)) {
    for (const MDOperand &Op : List->operands()) {
      const auto *Tuple = dyn_cast_or_null<MDNode>(Op.get());
      if (!Tuple)
        return gammaError("!hls.partition.args must list metadata tuples");
      Expected<PartitionSpec> S = parsePartitionSpec(Tuple);
      if (!S)
        return S.takeError();
      if (!S->ArgNo || *S->ArgNo >= F.arg_size())
        return gammaError("argument index missing or out of range in @" +
                          F.getName());
      ArgSpecs[*S->ArgNo] = *S;
    }
  }

  for (const Argument &Arg : F.args()) {
    if (!Arg.getType()->isPointerTy())
      continue;
    if (const PartitionSpec *Ov = FindOverride(&Arg)) {
      if (Error E = Env.add(&Arg, *Ov, DL))
        return std::move(E);
      continue;
    }
    auto It = ArgSpecs.find(Arg.getArgNo());
    if (It != ArgSpecs.end())
      if (Error E = Env.add(&Arg, It->second, DL))
        return std::move(E);
  }

  return std::move(Env);
}

//===----------------------------------------------------------------------===//
// Bank mapping and predicates
//===----------------------------------------------------------------------===//

z3::expr bankIndex(const ArrayInfo &A, const z3::expr &Offset) {
  assert(Offset.is_bv() && "flat GEP offsets are bit-vectors");
  z3::context &C = Offset.ctx();
  const unsigned W = Offset.get_sort().bv_size();
  // Unsigned arithmetic matches the bank-select logic of the address
  // decoder; offsets of executed accesses are additionally constrained to
  // [0, M) by the in-bounds facts placed in π.
  switch (A.Scheme) {
  case PartitionScheme::Cyclic:
    // Interleaved round-robin: element e lives in bank e mod N.
    return z3::urem(Offset, C.bv_val(A.Factor, W));
  case PartitionScheme::Block:
    // Contiguous chunks: element e lives in bank ⌊e / B⌋, B = ⌈M/N⌉.
    return z3::udiv(Offset, C.bv_val(A.blockSize(), W));
  case PartitionScheme::Complete:
    // Dissolved address space: one register per element, k = e.
    return Offset;
  }
  llvm_unreachable("unknown partition scheme");
}

void BankPredicate::print(raw_ostream &OS) const {
  OS << "bank(" << Index.to_string() << ", @" << Array->Name << ", "
     << Array->Factor << ")";
}

SpatialHeap materializeBank(z3::context &Ctx, const ArrayInfo &A, uint64_t K) {
  assert(A.Capacity && "materialisation needs a known capacity M");
  const uint64_t M = *A.Capacity;
  const uint64_t N = A.Factor;
  const unsigned DataW =
      static_cast<unsigned>(std::clamp<uint64_t>(A.ElemBytes * 8, 1, 64));

  SpatialHeap H = SpatialHeap::emp();
  // Each cell is joined with `*`, so the unfolding is literally the iterated
  // separating conjunction of the paper's definition.
  auto Cell = [&](uint64_t Offset, uint64_t J) {
    Loc L = Loc::make(Ctx.bv_val(A.Base + Offset, kAddrWidth), A.ArrID);
    std::string VName =
        A.Name + ".v_" + std::to_string(J) + "_" + std::to_string(K);
    Val V = Val::scalar(Ctx.bv_const(VName.c_str(), DataW));
    StarResult R = SpatialHeap::star(H, SpatialHeap::pointsTo(L, V));
    assert(!R.collapsed() && "a single bank never owns a cell twice");
    H = std::move(*R.Heap);
  };

  switch (A.Scheme) {
  case PartitionScheme::Cyclic: // α + j·N + k ↦ v_{j,k}
    if (K < M)
      for (uint64_t J = 0; J <= (M - 1 - K) / N; ++J)
        Cell(J * N + K, J);
    break;
  case PartitionScheme::Block: { // α + k·B + j ↦ v_{j,k}
    const uint64_t B = A.blockSize();
    if (K * B <= M - 1)
      for (uint64_t J = 0; J <= std::min(B - 1, M - 1 - K * B); ++J)
        Cell(K * B + J, J);
    break;
  }
  case PartitionScheme::Complete: // α + k ↦ v_k
    if (K < M)
      Cell(K, 0);
    break;
  }
  return H;
}

Error verifyCompositionRule(z3::context &Ctx, const ArrayInfo &A,
                            uint64_t MaxCells) {
  if (!A.Capacity || *A.Capacity > MaxCells)
    return Error::success();
  const uint64_t M = *A.Capacity;

  SpatialHeap Arr = SpatialHeap::emp();
  for (uint64_t K = 0; K < A.Factor; ++K) {
    SpatialHeap Bank = materializeBank(Ctx, A, K);

    // 𝔅 and bank(k) must describe the same hardware: every cell owned by
    // bank(k) has to be routed back to k by the mapping function.
    for (const PointsTo &C : Bank.cells()) {
      uint64_t Addr = 0;
      [[maybe_unused]] bool IsNum = C.Address.term().is_numeral_u64(Addr);
      assert(IsNum && "materialised addresses are numerals");
      z3::expr Routed =
          bankIndex(A, Ctx.bv_val(Addr - A.Base, kAddrWidth)).simplify();
      uint64_t RK = 0;
      if (!Routed.is_numeral_u64(RK) || RK != K)
        return gammaError("mapping function routes offset " +
                          Twine(Addr - A.Base) + " of @" + A.Name +
                          " away from bank " + Twine(K));
    }

    // arr(A) = bank(0) * ... * bank(N-1): an overlap is P * P ⇒ ⊥.
    StarResult R = SpatialHeap::star(Arr, Bank);
    if (R.collapsed())
      return gammaError("bank " + Twine(K) + " of @" + A.Name +
                        " overlaps a lower bank (P * P => false)");
    Arr = std::move(*R.Heap);
  }

  if (Arr.size() != M)
    return gammaError("banks of @" + A.Name + " cover " + Twine(Arr.size()) +
                      " cells, expected M = " + Twine(M));
  return Error::success();
}

} // namespace hls_sv

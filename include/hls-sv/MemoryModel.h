//===- MemoryModel.h - Dual-state Separation Logic memory model -*- C++ -*-===//
//
// HLS Spatial Verification Framework
//
// Paper §3.1 "The Spatial Memory Heap and Store". The program state is split
// into two strictly isolated components (Dual-State Isolation):
//
//   * MonotonicStore  s : Reg ⇀ Val   — pure arithmetic state over SSA
//                                        registers; grows monotonically.
//   * SpatialHeap     h : L   ⇀ Val   — physical on-chip memory; the only
//                                        place where spatial ownership lives.
//
// The domain distinction L ⊂ Val is enforced by the type system: `Loc`
// publicly derives from `Val` (every address is a storable value, so the
// upcast is implicit), but a `Val` can only become a `Loc` through the checked
// refinement `Loc::fromVal` (not every value — loop counters, i1 predicates,
// data words — is a dereferenceable address).
//
//===----------------------------------------------------------------------===//
#ifndef HLS_SV_MEMORYMODEL_H
#define HLS_SV_MEMORYMODEL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <z3++.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hls_sv {

//===----------------------------------------------------------------------===//
// Val — storable hardware values
//===----------------------------------------------------------------------===//

/// A storable hardware value. During compile-time symbolic execution every
/// value is a Z3 term: a bit-vector for integers and addresses (matching the
/// two's-complement semantics of LLVM integer types) or a Bool for i1.
class Val {
public:
  enum class Kind { Scalar, Address };

  /// Inject a pure arithmetic term into Val \ L.
  static Val scalar(z3::expr Term) {
    return Val(std::move(Term), Kind::Scalar, nullptr);
  }

  const z3::expr &term() const { return Term; }
  Kind kind() const { return K; }
  bool isAddress() const { return K == Kind::Address; }

  /// The array identifier A ∈ ArrID whose footprint contains this address;
  /// null for scalars.
  const llvm::Value *region() const { return Region; }

  void print(llvm::raw_ostream &OS) const;

protected:
  Val(z3::expr Term, Kind K, const llvm::Value *Region)
      : Term(std::move(Term)), K(K), Region(Region) {}

private:
  z3::expr Term;
  Kind K;
  const llvm::Value *Region;
};

//===----------------------------------------------------------------------===//
// Loc — physical addresses, L ⊂ Val
//===----------------------------------------------------------------------===//

/// A physical address ℓ ∈ L. Public inheritance is the subset inclusion
/// L ⊂ Val; the converse direction is only available through `fromVal`.
class Loc : public Val {
public:
  /// The address α + offset inside the footprint of array `Region`.
  /// `Address` must be a bit-vector term.
  static Loc make(z3::expr Address, const llvm::Value *Region);

  /// Checked refinement Val → L. Returns std::nullopt for scalars: a value
  /// outside L cannot be dereferenced into the heap.
  static std::optional<Loc> fromVal(const Val &V);

private:
  Loc(z3::expr Address, const llvm::Value *Region)
      : Val(std::move(Address), Kind::Address, Region) {}
};

//===----------------------------------------------------------------------===//
// MonotonicStore — s : Reg ⇀ Val
//===----------------------------------------------------------------------===//

/// The store binds LLVM virtual registers (and global identifiers) to values.
/// Because LLVM IR is in SSA form, each register is defined exactly once, so
/// the store only grows: the class deliberately offers no update or erase
/// operation, which removes the s → s' state transition from the semantics.
class MonotonicStore {
public:
  enum class BindStatus {
    Bound,                 ///< Reg ∉ dom(s); s := s ∪ {Reg ↦ V}.
    AlreadyBoundIdentical, ///< Memoised re-derivation of the same term.
    ImmutabilityViolation, ///< Would overwrite s(Reg): forbidden by SSA.
    NotARegister           ///< Constants are not in Reg; never stored.
  };

  /// Extend the store. Rebinding a register to a structurally different term
  /// is rejected because it would contradict SSA immutability.
  [[nodiscard]] BindStatus bind(const llvm::Value *Reg, const Val &V);

  /// s(Reg), or std::nullopt when Reg ∉ dom(s).
  std::optional<Val> lookup(const llvm::Value *Reg) const;

  bool contains(const llvm::Value *Reg) const { return Map.count(Reg) != 0; }
  size_t size() const { return Map.size(); }

  /// Reg = LLVM virtual registers (instructions, arguments) and global
  /// identifiers (paper §3, "Formal Syntax").
  static bool isRegister(const llvm::Value *V);

private:
  llvm::DenseMap<const llvm::Value *, Val> Map;
};

//===----------------------------------------------------------------------===//
// SpatialHeap — h : L ⇀ Val
//===----------------------------------------------------------------------===//

/// E ↦ F: exclusive ownership of the single physical location E.
struct PointsTo {
  Loc Address;
  Val Contents;
};

/// How two addresses relate, decided without the SMT oracle where possible.
enum class AliasVerdict {
  Disjoint,    ///< Provably distinct (different arrays or distinct numerals).
  MustAlias,   ///< Provably the same location.
  Undetermined ///< Symbolic; needs a pure obligation ℓ1 ≠ ℓ2.
};

AliasVerdict classifyLocs(const Loc &A, const Loc &B);

struct StarResult;

/// A finite partial map from physical addresses to values. Heaps are only
/// combined through the separating conjunction, which is where disjointness
/// (h1 ⊥ h2 ⇔ dom(h1) ∩ dom(h2) = ∅) is checked.
class SpatialHeap {
public:
  /// emp: owns no physical memory.
  static SpatialHeap emp() { return SpatialHeap(); }

  /// The singleton heaplet ℓ ↦ v.
  static SpatialHeap pointsTo(Loc L, Val V);

  /// Σ1 * Σ2. Concrete overlaps collapse immediately (P * P ⇒ ⊥); symbolic
  /// address pairs produce pure obligations ℓi ≠ ℓj for the SMT oracle.
  static StarResult star(const SpatialHeap &H1, const SpatialHeap &H2);

  /// h(ℓ) under provable address equality.
  std::optional<Val> lookup(const Loc &L) const;

  bool isEmp() const { return Cells.empty(); }
  size_t size() const { return Cells.size(); }
  llvm::ArrayRef<PointsTo> cells() const { return Cells; }

  void print(llvm::raw_ostream &OS) const;

private:
  void append(const PointsTo &C);

  std::vector<PointsTo> Cells;
  /// Index of cells whose address is a numeral. Γ gives every array a
  /// disjoint physical range, so a numeral is a global address in L.
  std::unordered_map<uint64_t, size_t> ConcreteIndex;
  /// Cells whose address is still symbolic.
  std::vector<size_t> SymbolicCells;
};

/// Result of composing two heaplets with `*`.
struct StarResult {
  /// std::nullopt ⇔ the composition collapsed to ⊥.
  std::optional<SpatialHeap> Heap;
  /// Pure side conditions ℓi ≠ ℓj under which the composition is defined.
  std::vector<z3::expr> Obligations;

  bool collapsed() const { return !Heap.has_value(); }
};

} // namespace hls_sv

#endif // HLS_SV_MEMORYMODEL_H

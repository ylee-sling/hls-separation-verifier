//===- BankPredicate.h - Γ(A) and polymorphic bank predicates ---*- C++ -*-===//
//
// HLS Spatial Verification Framework
//
// Paper §3.2 "Memory Bank Predicates" and §4.1 (bank mapping function 𝔅).
//
//   Γ : ArrID → L × ℕ × {Cyclic, Block, Complete},   Γ(A) = (α, M, P)
//
//   bank(k, A, N) ≜ bank_cyc(k, A, N)   if P = Cyclic
//                   bank_blk(k, A, N)   if P = Block
//                   bank_comp(k, A, M)  if P = Complete
//
//   𝔅(e, N)       ≜ e mod N             if P = Cyclic
//                   ⌊e / B⌋, B = ⌈M/N⌉  if P = Block
//                   e                   if P = Complete   (N = M)
//
// ---------------------------------------------------------------------------
// Γ metadata schema (key/value tuples; keys are MDStrings)
// ---------------------------------------------------------------------------
//   Globals and allocas carry an attachment named !hls.partition:
//
//     @A = global [64 x i32] zeroinitializer, !hls.partition !0
//     %T = alloca [16 x float], align 4,       !hls.partition !1
//     !0 = !{!"scheme", !"cyclic",   !"factor", i32 4}
//     !1 = !{!"scheme", !"complete"}
//
//   Pointer arguments are described by a function attachment
//   !hls.partition.args listing one tuple per argument. M and the element
//   size cannot be recovered from an opaque `ptr`, so they are required for
//   explicitly partitioned arguments:
//
//     define void @k(ptr %A) !hls.partition.args !2 { ... }
//     !2 = !{!3}
//     !3 = !{!"arg", i32 0, !"scheme", !"block", !"factor", i32 4,
//            !"capacity", i64 64, !"elem_bytes", i64 4}
//
//   Optional keys: "capacity" (overrides M inferred from the IR type),
//   "elem_bytes", "base" (α; otherwise allocated disjointly by Γ).
//
//   For quick experiments the same information can be given on the command
//   line:  -hls-sv-partition=<name>:<scheme>:<N>[:<M>[:<elem_bytes>]]
//   (argument and alloca names survive only with -fno-discard-value-names).
//
//   Allocations without partition information are modelled as a monolithic
//   single-port SRAM (Cyclic, N = 1): every bank index is 0.
//
//===----------------------------------------------------------------------===//
#ifndef HLS_SV_BANKPREDICATE_H
#define HLS_SV_BANKPREDICATE_H

#include "hls-sv/MemoryModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <z3++.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class DataLayout;
class Function;
class MDNode;
class Value;
} // namespace llvm

namespace hls_sv {

inline constexpr const char *kPartitionMDName = "hls.partition";
inline constexpr const char *kPartitionArgsMDName = "hls.partition.args";

enum class PartitionScheme { Cyclic, Block, Complete };

llvm::StringRef toString(PartitionScheme P);
std::optional<PartitionScheme> parsePartitionScheme(llvm::StringRef S);

/// One partition directive, as read from metadata or the command line, before
/// it is resolved against the IR type of the allocation.
struct PartitionSpec {
  std::optional<PartitionScheme> Scheme;
  std::optional<uint64_t> Factor;    // N
  std::optional<uint64_t> Capacity;  // M
  std::optional<uint64_t> ElemBytes; // sizeof(element)
  std::optional<uint64_t> Base;      // α
  std::optional<unsigned> ArgNo;     // only in !hls.partition.args tuples
};

/// Parse one key/value metadata tuple (see schema above).
llvm::Expected<PartitionSpec> parsePartitionSpec(const llvm::MDNode *MD);

/// A command-line override: <name>:<scheme>:<N>[:<M>[:<elem_bytes>]].
struct GammaOverride {
  std::string Name;
  PartitionSpec Spec;
};
llvm::Expected<GammaOverride> parseGammaOverride(llvm::StringRef Text);

/// Γ(A) = (α, M, P), together with N and the element size used to turn the
/// byte offsets computed by getelementptr into flat element offsets.
struct ArrayInfo {
  const llvm::Value *ArrID = nullptr; ///< A ∈ ArrID: global, alloca or arg.
  std::string Name;
  uint64_t Base = 0;                ///< α ∈ L (element-granular).
  std::optional<uint64_t> Capacity; ///< M; unknown for unsized pointers.
  PartitionScheme Scheme = PartitionScheme::Cyclic; ///< P
  uint64_t Factor = 1;                              ///< N (= M for Complete).
  uint64_t ElemBytes = 1; ///< sizeof(element) in bytes.
  bool Explicit = false;  ///< false ⇒ implicit monolithic SRAM.

  /// B = ⌈M/N⌉; only meaningful for Block partitioning.
  uint64_t blockSize() const;
  void print(llvm::raw_ostream &OS) const;
};

/// The Static Hardware Environment Γ. It is the formal counterpart of the
/// module's symbol table plus HLS partition metadata.
class StaticHardwareEnv {
public:
  /// Build Γ for F: module globals, F's allocas and F's pointer arguments.
  static llvm::Expected<StaticHardwareEnv>
  build(const llvm::Function &F, llvm::ArrayRef<GammaOverride> Overrides = {});

  /// Γ(A) for an explicitly partitioned or previously created entry.
  const ArrayInfo *lookup(const llvm::Value *ArrID) const;

  /// Γ(A), creating the implicit monolithic entry (N = 1) for a static
  /// allocation without partition metadata. Returns null if ArrID is not a
  /// global variable, alloca or pointer argument.
  const ArrayInfo *lookupOrImplicit(const llvm::Value *ArrID,
                                    const llvm::DataLayout &DL);

  llvm::ArrayRef<std::unique_ptr<ArrayInfo>> entries() const { return Entries; }

private:
  llvm::Error add(const llvm::Value *ArrID, const PartitionSpec &Spec,
                  const llvm::DataLayout &DL);
  llvm::Error assignBase(ArrayInfo &A, std::optional<uint64_t> Requested);

  std::vector<std::unique_ptr<ArrayInfo>> Entries; // stable addresses
  llvm::DenseMap<const llvm::Value *, ArrayInfo *> Index;
  uint64_t NextBase = 0;
};

//===----------------------------------------------------------------------===//
// Bank predicates
//===----------------------------------------------------------------------===//

/// 𝔅(expr, N): route a flat element offset (a bit-vector term over SSA
/// registers) to its physical bank index, polymorphically in Γ(A).P.
z3::expr bankIndex(const ArrayInfo &A, const z3::expr &Offset);

/// bank(k, A, N): exclusive spatial ownership of the k-th bank of A. The
/// index k is a symbolic expression over the SSA store, not a constant, so
/// non-affine terms such as (x·Y + y) mod N are carried as-is to the oracle.
struct BankPredicate {
  const ArrayInfo *Array;
  z3::expr Index; ///< k

  void print(llvm::raw_ostream &OS) const;
};

/// Unfold bank(k, A, N) for a concrete k into the paper's definition as an
/// iterated separating conjunction of points-to cells:
///   bank_cyc : ∗_{j=0}^{⌊(M−1−k)/N⌋}       α + j·N + k ↦ v_{j,k}
///   bank_blk : ∗_{j=0}^{min(B−1, M−1−k·B)}  α + k·B + j ↦ v_{j,k}
///   bank_comp:                              α + k       ↦ v_k
/// An index beyond the populated banks unfolds to emp. Requires a known M.
SpatialHeap materializeBank(z3::context &Ctx, const ArrayInfo &A, uint64_t K);

/// Array composition rule: arr(A) ≜ ∗_{k=0}^{N−1} bank(k, A, N).
/// Checks, for the concrete instance described by Γ(A), that the bank
/// predicates compose without collapse, cover exactly M cells, and agree with
/// 𝔅 (every cell of bank(k) is routed to k). Arrays with unknown M or more
/// than MaxCells elements are skipped.
llvm::Error verifyCompositionRule(z3::context &Ctx, const ArrayInfo &A,
                                  uint64_t MaxCells = 4096);

} // namespace hls_sv

#endif // HLS_SV_BANKPREDICATE_H

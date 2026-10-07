# HLS Spatial Verification Framework

An out-of-tree LLVM new-pass-manager plugin. After loop unrolling it proves that
concurrent accesses to partitioned on-chip arrays never hit the same single-port
bank, by reducing the Separation Logic Conflict-Free Unrolling (CFU) condition to
a Z3 query. Reference paper: `docs/2607.07126v1.pdf` (§3 memory model, §4 CFU,
§5 translation T and fallback M(S), §6 soundness).

## Current state

- `include/hls-sv/` and `src/` contain the four core modules. They were written
  against LLVM 18 / Z3 4.8+ APIs but have **never been compiled**. Expect small
  API mismatches; fix them without changing semantics.
- Nothing else exists yet: no CMake, no tests, no scheduler signaling.

## Toolchain

- LLVM 18 (`llvm-18-dev`, `llvm-18-tools` for FileCheck), `libz3-dev`, CMake >= 3.20,
  Ninja, `lit` (`pip install lit`). C++17.
- Keep LLVM 17–20 compatibility: guard differing APIs with `LLVM_VERSION_MAJOR`
  (one guard already exists for `registerOptimizerLastEPCallback`, one for
  `or disjoint` / `zext nneg`).

## Layout

```
CLAUDE.md  CMakeLists.txt  README.md  docs/2607.07126v1.pdf
include/hls-sv/  MemoryModel.h  BankPredicate.h  SMTTranslator.h
                 SpatialVerificationPass.h  SchedulerSignal.h (phase 3)
src/             matching .cpp files
tests/lit/       lit.cfg.py, lit.site.cfg.py.in, one .ll per acceptance row
tests/unit/      C++ unit tests (plain asserts or GoogleTest from LLVM)
```

Includes are written as `#include "hls-sv/<Name>.h"`; keep the `hls-sv/` prefix.

## Module map

| Module | Role | Paper |
| --- | --- | --- |
| MemoryModel | `Val`, `Loc` (L ⊂ Val via `Loc::fromVal`), `MonotonicStore` s : Reg ⇀ Val, `SpatialHeap` h : L ⇀ Val with `star` (P * P ⇒ ⊥) | §3.1 |
| BankPredicate | Γ parser (metadata + `-hls-sv-partition`), `bankIndex` = 𝔅, `materializeBank`, `verifyCompositionRule` (arr(A) = ∗ bank(k)) | §3.2, §4.1 |
| SMTTranslator | `SymbolicEvaluator` (SSA → QF_BV, memoised in the store), `translate` = T, `discharge` + M(S) | §5 |
| SpatialVerificationPass | `InstVisitor` GEP interception, region grouping, path guards, printer, plugin registration | Fig. 1 |

## Non-negotiable invariants

Do not "simplify" any of these away. If a test or a fix seems to require breaking
one, stop and ask.

1. **Validity, not satisfiability.** Z3 receives ¬VC = π ∧ ⋁(Aᵢ ∧ Aⱼ ∧ kᵢ = kⱼ).
   `unsat` ⇒ VC valid ⇒ `VCStatus::Proven` (the paper's "Sat") ⇒ Parallel.
   `sat` ⇒ `Refuted` with a counterexample. Never assert the VC itself and treat
   `sat` as safe — that approves conflicting accesses.
2. **M(S):** only `Proven` → Parallel. `Refuted`, `Unknown`, `Timeout`, and any
   `z3::exception` → Sequential.
3. **QF_BV at LLVM bit widths.** No Int sort: integers ignore two's-complement
   wrap-around (i64 x = 2⁶⁴−1 gives x mod 3 = (x+1) mod 3 = 0).
4. **Deterministic T_out** = Z3 `rlimit`. The wall-clock timeout stays off by
   default and is never used in tests.
5. **Dual state.** `MonotonicStore` never rebinds a register (SSA). The heap's
   domain is `Loc` only; a `Val` becomes a `Loc` only through `Loc::fromVal`.
6. **GEP interception only.** Offsets come from `GEPOperator::collectOffset` along
   the GEP chain. Never re-derive C/C++ multi-dimensional flattening.
7. **Sound abstraction.** Anything not interpreted becomes a fresh symbol (havoc).
   Facts in π or in a claim guard Aᵢ must be *necessary* conditions (dominating
   branch edges, llvm.assume, UB-freedom: in-bounds, nsw/nuw/exact/disjoint/nneg).
   Dropping a fact is always safe; adding an unjustified one is a soundness bug.
8. **Unresolved accesses** (unknown base, memory-touching calls, element-straddling
   widths) taint their whole region → Sequential.
9. **The analysis never mutates IR.** IR annotation belongs to a separate
   transform pass (SchedulerSignal).
10. **Exceptions stay inside the plugin.** Z3's C++ API throws; LLVM is built with
    `-fno-exceptions`. Catch at the analysis boundary.
11. **Strict exclusive ownership** (single-port banks): a load and a store of the
    same element in one region collapse. This is intended (paper §4.2;
    fractional permissions are future work). Do not special-case it.

## Build

- Plugin: `add_library(HLSSpatialVerify MODULE ...)` with `PREFIX ""`, so the
  artifact is `build/HLSSpatialVerify.so`.
- Do **not** link LLVM component libraries into the plugin; `opt` provides those
  symbols, and linking them again registers every `cl::opt` twice. Do link Z3
  (`find_package(Z3 CONFIG)` with a `find_library`/`find_path` fallback).
- Compile with `-fexceptions`; use `-fno-rtti` unless `LLVM_ENABLE_RTTI`.
- Unit-test executables are separate targets that *do* link LLVM and Z3.

```
cmake -S . -B build -G Ninja -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
```

## Running

```
opt-18 -load-pass-plugin build/HLSSpatialVerify.so \
       -passes='print<hls-spatial-verify>' -disable-output in.ll
```

Options: `-hls-sv-partition=<name>:<scheme>:<N>[:<M>[:<elem_bytes>]]`,
`-hls-sv-rlimit=N`, `-hls-sv-dump-vc`, `-hls-sv-refine-pairs`,
`-hls-sv-max-claims=N`, `-hls-sv-check-gamma`. The Γ metadata schema is
documented at the top of `include/hls-sv/BankPredicate.h`.

## Testing rules

- lit + FileCheck on the printer output, e.g.
  `CHECK: status: Proven (paper: Sat) -> Parallel`.
- Prefer small hand-written IR committed as `.ll`. If you derive IR from C with
  `clang-18 -O2`, commit the `.ll`, not the C, so tests don't drift with clang.
- Use load-only kernels (reductions) for Proven cases; read-modify-write bodies
  collapse by design (invariant 11).
- Force T_out with a tiny `-hls-sv-rlimit`, never with wall-clock time. Check the
  query is not already decided syntactically.
- Every soundness-relevant case has a Parallel twin and a Sequential twin. The
  conflict twins are the regression guard for invariant 1.

## Acceptance matrix (tests/lit, one file per row)

| Test | Setup | Expected |
| --- | --- | --- |
| cyclic_ok | `@A[64]` cyclic N=4; loads A[i], A[i or 1], A[i or 2], A[i or 3] (`or disjoint`, i steps by 4) | Proven → Parallel |
| cyclic_conflict | same accesses, N=2 | Refuted → Sequential, witness printed |
| block_ok | M=64 block N=4; A[i], A[i+16], A[i+32], A[i+48], i < 16 | Proven |
| block_conflict | block N=4; A[i..i+3] | Refuted |
| complete_ok | M=8 complete; 8 constant offsets | Proven, separated syntactically |
| multidim | `[8 x [8 x i32]]` cyclic N=8; B[x][0..7] loads | Proven (offset from GEP) |
| nonaffine_conflict | A[x*y] and A[x*y+4], cyclic N=4 | Refuted |
| undecidable_fallback | non-linear index pair (e.g. (x*y) urem 7 vs (x*y+1) urem 7, N=7) with tiny rlimit | Timeout (paper: Tout) → Sequential |
| guarded | same-bank accesses in mutually exclusive if/else arms | Proven (guards contradict) |
| vector | one `<4 x i32>` load; cyclic N=4 / N=2 | Proven / Refuted |
| unresolved | pointer from `select` of two arrays | "cannot map to Gamma" → Sequential |
| rmw_collapse | A[i] = A[i] * 3 | Refuted (intended) |
| gamma_error | block partition on a `ptr` arg without capacity | environment error → Sequential |
| monolithic | unannotated array, two accesses | Refuted (N = 1) |

## Working agreement

- Work in the phase you are given and stop at its end with a report.
- List every source edit with a one-line reason. Design concerns go in the report
  as findings; do not redesign on your own.
- Do not edit `docs/`. Do not commit; the user reviews and commits each phase.

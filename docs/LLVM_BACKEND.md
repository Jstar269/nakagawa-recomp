# LLVM IR backend (opt-in)

Status: first stage, opt-in (issue #330). The C backend is the default and stays the
reference until the LLVM backend matches it everywhere.

Nakagawa translates guest MIPS into C and compiles the C with GCC (or Clang, #317). The
LLVM backend lowers the same guest functions straight to LLVM IR instead, so the backend,
not a C compiler, decides how delay slots, guest register state, escapes to the runtime
and memory accesses become host code. The maintainer compared LLVM IR, Cranelift and a
project-authored encoder for #330 and chose LLVM IR as the long-term path.

```powershell
mingw32-make --no-print-directory cosim-selftest-llvm    # the AOT/interpreter gate, LLVM lane
mingw32-make --no-print-directory cosim-mutants-llvm     # the backend's mutation campaign
mingw32-make --no-print-directory production-smoke CODEGEN_BACKEND=llvm
```

Any build takes `CODEGEN_BACKEND=llvm`. It needs Clang and the LLVM tools (`llvm-link`;
`opt` for the tests) from MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-clang
mingw-w64-ucrt-x86_64-llvm-tools` ([`SETUP.md`](SETUP.md#optional-llvm-backend)).

## Pipeline

```text
analyzer ──> codegen.function_flow ──> C emitter (emit_function) ─────> <g>_recomp_<n>.c ──> GCC ──┐
             (ownership: blocks,   │                                     (declares, registers      │
              labels, continuations)└─> lowering plan (per function)      and wraps LLVM bodies)    ├─> link
                                        tools/lowering_plan.py                                      │
                                          └─> LLVM renderer ──> <g>_recomp_<n>.ll                   │
                                              tools/codegen_llvm.py     │                           │
                    src/rt/llvm_abi.c ──Clang -emit-llvm──> llvm_abi.bc ┴─> llvm-link ──> Clang -c ─┘
```

Every function goes through the C emitter first. If the C emitter refuses an encoding, the
function stays its fail-closed stub whatever the backend. If it translates, and the
backend is `llvm`, `tools/lowering_plan.py` builds the function's **lowering plan** from
the same inputs the C emitter uses (`function_flow`'s instruction set, native labels and
continuation exits, the entry catalog, resume owners and dispatch boundaries) and
`tools/codegen_llvm.py` renders that plan. The plan is backend-neutral: each instruction
is an `Op` naming its semantic operation and operands, and each control instruction a
`Transfer` with its kind (checked CALL, dispatched CALL, TAIL record, return, in-body
edge), target, symbol, link register, condition and delay slot resolved. The renderer
decodes no MIPS and makes no transfer decision.

### One representation for both backends

The design rule (maintainer, 2026-10-29) is that every backend lowers from the same analysis
representation, so a semantic fix lands once and the backends cannot drift. Where that
stands in this stage:

- The LLVM backend lowers only from the plan.
- The C emitter does not consume the plan yet: `emit_function` and `effect()` still make
  the same decisions and spell each operation as C text themselves. Until they render the
  plan, `tools/test_lowering_plan.py` compares every plan with what `emit_function` emits
  for the same function over a real codegen run (native labels, continuation exits,
  checked CALLs, dispatched CALLs, TAIL records, returns and preemption points), so a rule
  changed on one side fails there.
- The plan is built from codegen's ownership, not from #305's analysis IR
  (`tools/analysis_ir.py`, PR #1642). That IR is a module-wide projection of the
  analyzer's CFG report and, by its own contract, advisory: codegen does not read it, so
  neither backend can lower from it today.

The next steps, in order: move `emit_function` onto the plan and `effect()`'s
per-instruction C onto `Op` (one decision site, two renderers); then build the plan from
the shared analysis IR once codegen consumes that IR (#305's successor), with the plan as
its per-function lowering view.

A function the backend does not lower keeps its C body unchanged, and the reason goes to
`<g>_recomp_llvm.json` (`codegen_llvm.py check-manifest` prints it). Nothing is dropped or
approximated silently.

## Structured IR

`tools/llvm_ir.py` builds IR with the data model of the LLVM C API and prints text only at
the end:

| `llvm_ir` | LLVM C API |
| --- | --- |
| `Module`, `Module.add_function`, `get_or_declare` | `LLVMModuleCreateWithName`, `LLVMAddFunction`, `LLVMGetNamedFunction` |
| `Function.append_basic_block` | `LLVMAppendBasicBlock` |
| `Builder.position_at_end`, `position_before` | `LLVMPositionBuilderAtEnd`, `LLVMPositionBuilderBefore` |
| `Builder.add`, `icmp`, `select`, `load`, `store`, `call`, `cond_br`, ... | `LLVMBuildAdd`, `LLVMBuildICmp`, `LLVMBuildSelect`, `LLVMBuildLoad2`, ... |
| `const`, `null` | `LLVMConstInt`, `LLVMConstPointerNull` |
| `Module.link_in` | `LLVMLinkModules2` |

Values are objects, not strings: an operand renders when the module prints, so a
symbol-map rename (`--symbol-map`) renames a function and every call to it. The builder
rejects what the verifier would reject, at the call that causes it: operand types that
differ, a non-`i1` branch condition, a call that does not match its callee, an instruction
after a terminator, a block with no terminator, a branch into another function.

## Semantics in the IR

Each lowered function is `void f_<addr>(ptr %s)`, the same symbol and `CpuState` ABI as
the C body. What the C path leaves to a C compiler is explicit here:

- **Guest register state** lives in `CpuState`. Every guest read is an `i32` load and
  every write a store at the field's byte offset, in program order; a write to `r0` emits
  nothing. `CPUSTATE_OFFSETS` lists the offsets, and an LLVM build's `<g>_recomp.c`
  carries a `_Static_assert` for each one and for `SR_CPUSTATE_ABI_VERSION`, so a layout
  change fails the build instead of corrupting state.
- **Spills.** Because no guest value is cached in an SSA register across an escape, there
  is nothing to spill or flush. Register promotion (#284's verified spans, as allocas that
  LLVM's `mem2reg` turns into SSA) is the next step; it needs a flush before each escape
  listed below and a mirror of every write in trace builds, as #284 does in C.
- **Escapes to the runtime** are calls that take `s`: the trace hooks around every
  instruction, the preemption point at entry and on backward branches, memory helpers, the
  checked CALL protocol (`sr_aot_return_boundary_begin`, the callee, the tail drain,
  `sr_aot_return_boundary_end`), `dispatch_call` for a computed or unknown target,
  `sr_tail_record` for every TAIL transfer (#855), the return capture of `jr $ra`, the HLE
  syscall boundary and `break`.
- **Delay slots** are emitted at their owning transfer. A branch condition and a computed
  `jr`/`jalr` target are read before the slot (PR #126); a branch-likely runs its slot only
  on the taken path; a slot that is also a branch target is emitted again under its own
  label and skipped on fall-through, as in C.
- **Memory** goes through the existing access helpers: bounds, VRAM coherence, watchpoints
  and store context all stay in one place.
- **Calls, tail calls and dispatch** follow the C emitter's transfer rules exactly: a known
  entry is a checked native CALL; an unknown or computed target is `dispatch_call` with an
  explicit resume PC; `j` to a known entry, `jr rs`, `jalr $zero` and a continuation exit
  are recorded TAIL transfers.
- **Undefined behavior** in LLVM terms never reaches the IR: variable shifts are masked to
  31 as the C does, rotates are shifts, `div`/`divu` select a safe divisor and then the
  PSP's defined results for division by zero and `INT_MIN / -1`, and `clz`/`clo` use
  `llvm.ctlz` with zero defined.

`src/rt/llvm_abi.c` gives each recomp.h macro or inline helper the IR calls a named entry
point whose body is that macro (`sr_llvm_r32` is `MEM_R32`, `sr_llvm_yield` is `SR_YIELD`,
and so on); it adds no semantics. The build compiles it to bitcode with the generated-code
flags and links only the functions a chunk uses into that chunk, internalized and
`always_inline`, so an LLVM body reaches the same helpers a C chunk expands, and a release
build keeps the trace hooks compiled out. `tools/test_codegen_llvm.py` compiles the header
with Clang and checks that every declaration matches the type the backend prints.

The two compilers then inline those helpers differently. GCC at `-O1` keeps the memory
helpers (`sr_r32`, `sr_w32_pc` and the narrower forms, with their VRAM aperture, watch and
store-context paths) as out-of-line calls; Clang at `-O1` inlines them at every guest load
and store. That is most of the LLVM build's extra object size (#330 has the numbers), and
splitting the helpers into an inline bounds check and an out-of-line slow path is listed
[below](#not-yet-done).

## Scope of the first stage

Lowered: the integer ALU and compares (including the Allegrex `max`/`min`, rotates, `ext`,
`ins`, `wsbh`, `wsbw`, `seb`, `seh`, `bitrev`, `clz`, `clo`), HI/LO multiply, divide and
multiply-accumulate, integer loads and stores (including `lwl`/`lwr`/`swl`/`swr` and
`ll`/`sc`), `cache` and `sync`, conditional branches and branch-likely, `j`, `jal`,
`jalr`, `jr`, the linked-branch CALL form (#968), continuations, the HLE syscall boundary
and `break`, and the stack census wrapper.

Kept in C, with the reason recorded: scalar FPU and VFPU instructions and their branches,
COP0 and `eret`, `--lle-cpu` (its synchronous exception lowering), `--static-verify`
assertions, `--stale-detect`'s `cache` hook, enhancement-package rewrite sites,
runtime-placed modules (their addresses are relocatable expressions), and the HST
profile's emission sites (`hst_emission_sites`, which a test re-derives from
`emit_function`).

## Verification

- `llvm-link` runs LLVM's verifier on each chunk's module and on the linked module before
  Clang sees it, and Clang verifies its IR input again; the tests run
  `opt -passes=verify` on the builder's output and on every chunk module of a real build.
- `cosim-selftest-llvm` runs the cosimulation corpus with every lowerable cell as an LLVM
  body in the AOT lane, compared with the production interpreter on the instruction trace,
  the ordered guest writes, the memory window and the state vector, and fails if fewer than
  20 functions were lowered.
- `cosim-mutants-llvm` damages the lowering one defect at a time (inverted branch, lost
  delay slot, branch-likely slot that always runs, missing HI write, missing load write,
  computed target read after the slot, `sc` without a link, wrong call resume PC); each
  must build, run and fail with a located divergence.
- `production-smoke`, `production-smoke-gap` and `profile-zero-e2e` run with
  `CODEGEN_BACKEND=llvm`.
- Private title routes (HST, PAC-MAN CE, Gravity Crash) compare an LLVM build's results
  with the C build's; they are recorded on #330, not in this repository.

## Build identity

`--backend=llvm` is part of the codegen profile, so switching it regenerates the sources.
In an LLVM build, the Clang and `llvm-link` version strings join the generated-code
profile, so a toolchain change rebuilds the LLVM objects and is never mixed with objects
from another toolchain. A C build's profiles are unchanged by this backend.

## Delivery, and the in-process stage

Delivery is the only part tied to external tools: print the module, `llvm-link` it with
the ABI bitcode, compile it with `clang -c`. The lowering only calls the builder. The next
stage replaces the delivery and nothing else:

1. Forward each builder call to the LLVM C API (`LLVMModuleCreateWithName`,
   `LLVMBuildAdd`, ...), from the native tool that runs codegen or through a thin binding.
2. Link the ABI bitcode with `LLVMLinkModules2` and run the `default<O1>` pipeline with
   `LLVMRunPasses`.
3. Emit objects with `LLVMTargetMachineEmitToFile`, or hand the module to ORC (`LLJIT`) to
   compile at load time for the player's first-run path
   ([`AOT_PRODUCTIZATION_ARCHITECTURE.md`](AOT_PRODUCTIZATION_ARCHITECTURE.md), option E).

LLVM is Apache-2.0 with the LLVM exception, which is compatible with GPL-3.0-or-later. The
current stage vendors nothing: it uses the MSYS2 toolchain the user installs. Any LLVM
header or library vendored or shipped later gets the usual pin, `PROVENANCE.md` entry,
`CREDITS.md` credit and admission.

## Not yet done

- The C emitter rendering the lowering plan, and the plan built from the shared analysis
  IR ([above](#one-representation-for-both-backends)).
- Lowering for the FPU, VFPU, COP0, `eret` and the `--lle-cpu` exception path.
- Register promotion inside the IR (above), and IR-side fast paths that do not depend on
  linking the ABI bitcode.
- Code size: the memory helpers split into an inline fast path and an out-of-line slow path
  (VRAM aperture, watch and store-context checks), so Clang does not inline the slow path
  at every access ([above](#semantics-in-the-ir)).
- Runtime-placed (relocatable) modules.
- The in-process stage, and arm64 hosts (#329).
- The `--stale-detect` `cache` hook. Stale-code records are kept per guest entry, and an
  LLVM body has the same entry, symbol and registration as the C body, so the detector
  already covers lowered bodies; only a function containing the hook stays in C.

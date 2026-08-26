# AOT ↔ interpreter cosimulation

A differential gate between the two ways this project can execute a guest
instruction: the statically translated native body produced by
[`tools/codegen.py`](../../tools/codegen.py), and the production interpreter floor
in [`src/rt/guest_interp.c`](../../src/rt/guest_interp.c).

It exists to answer one question precisely:

> **Where is the FIRST semantic difference between native AOT execution and the
> production interpreter, on the same guest bytes?**

```bash
mingw32-make cosim-selftest
```

```bash
mingw32-make cosim-mutants
```

## How one build produces two lanes

`generate.py` emits a source-owned synthetic PSP module into the ignored build
tree. The ordinary pipeline (`prxload.py` → `codegen.py`) turns it into a
relocated guest image plus real generated C. Both lanes then run *those same
bytes*:

| lane | how it executes | how it is selected |
| --- | --- | --- |
| `AOT` | the generated `f_<addr>` body | the cell is present in the dispatch table |
| `INTERP` | `sr_guest_interp_run()` over the loaded image | the cell is absent from the dispatch table |

Lane `INTERP` is not a test mode. It is the same seam the AOT-gap smoke reaches
at build time with `--omit-aot`, selected at run time so one build can compare
every cell. Executable-span ownership lives outside the dispatch table, so
dropping the table leaves lane `INTERP` with exactly the ownership lane `AOT`
has and none of the native bodies.

Both lanes enter through the real `dispatch()` core, so the tier-selection
policy, the executable-ownership predicate and the miss path are production code
in both runs.

## What is compared

Four independent channels, ordered from most localizing to least. Each has been
shown to be the *sole* killer of at least one mutant, so none is decorative.

1. **Canonical instruction trace** — `sr_trace_open()` / `sr_begin()` / `sr_end()`,
   the per-instruction record the generated code already emits
   ([`tools/TRACE_FORMAT.md`](../../tools/TRACE_FORMAT.md)). `guest_interp.c` now emits the same records in the
   same order, so the first differing line names the exact guest PC, the raw
   instruction word and the registers that changed.
2. **Ordered guest writes** — captured through `sr_note_mem_write()`, the
   production last-writer hook every `MEM_W*_PC` store already reaches. Each
   record carries `pc`, address, width and a real before/after pair.
3. **Guest memory window** — the whole scratch + stack region compared byte for
   byte, so a write outside the log's view still fails.
4. **Architectural state vector** — every architecturally visible 32-bit field
   (`r0..r31`, `hi`, `lo`, `fcr31`, `fpcond`, `f0..f31`, `v0..v127`,
   `vfpuCtrl0..15`, `status`, `next_pc`, `in_delay_slot`) as one ordered list, so
   a report names the first differing field rather than "states differ".

`CpuState.pc` is deliberately **not** in the vector — see below.

## The precise PC contract

`CpuState.pc` is not a shared architectural field between the lanes, and the gate
would be dishonest if it compared them directly. What each lane actually
guarantees is asserted per lane, so a change to either fails this gate instead of
quietly redefining what `pc` means:

Generated code does not maintain a **per-instruction** architectural PC. That is not
the same as never writing `pc`, and the difference matters: `tools/codegen.py` does
assign `s->pc` at a VFPU interpreter fallback, at a conditional branch whose target is
outside the current function, and in the profile stubs. None of those shapes occurs in
this fixture, which is why the per-lane assertion below is exact for these cells — and
why adding a cell that contains one would have to revisit it rather than silently
weaken it.

| concept | lane `INTERP` | lane `AOT` |
| --- | --- | --- |
| current instruction | `pc`, advanced per instruction | **not maintained per instruction.** Each instruction's address is a literal compiled into its `sr_begin()` call — which is why the trace, not `CpuState`, localizes an AOT divergence |
| next instruction | `next_pc` unmaintained | `next_pc` unmaintained |
| branch owner | the branch owns its delay slot: condition and transfer target are read at the branch, the link register is written before the slot, and the AOT tier is not reconsulted at `pc+4` | same ordering, emitted statement by statement |
| delay slot | `in_delay_slot` unmaintained | `in_delay_slot` unmaintained |
| AOT handoff | — | `jr $ra` **is** the host return; no `pc` write, and the architectural destination is `$ra` |
| interpreter handoff | writes `pc = destination` before dispatching into the registered native body | — |
| exception future | asserted to leave `next_pc` and `in_delay_slot` at their seeded values | same, so a future COP0 BD/EPC model can define them without colliding with an existing consumer |

The comparator normalizes both lanes to a single **handoff target** — `$ra` for
`AOT`, `pc` for `INTERP` — and requires them to agree. No field was repurposed
and no consumer of `pc` changed.

That normalization has a precondition, and the gate now checks it rather than assuming
it: lane `AOT`'s `$ra` is read **at termination**, so it is the transfer's latched
target only while nothing rewrote `$ra` afterwards. A return delay slot that writes
`$ra` would make the comparator report a destination control never reached. The
canonical trace already records which registers each instruction changed, so the final
delay slot's line is inspected and the cell fails with that explanation instead of
silently comparing the wrong thing.

## Cross-tier cells

`xcall` and `xtail` add a third lane, `MIXED`: every native body **except one**. That is
the seam the production AOT-gap floor actually crosses — native caller, dispatch miss,
production interpreter, back — and it is the one shape a whole-cell lane cannot express.
`install_lane_mixed()` refuses to run if the omitted address was never registered, so a
cell cannot degrade into a second all-native run and pass vacuously.

* `xtail` is the **control**: a tail transfer into an interpreted middle that stores and
  tail-transfers on to a registered entry which reads the store back. It is an ordinary
  PASS, and it is the shape `fixtures/production_smoke` (aot-gap) and the platform ladder
  already use.
* `xcall` is the **returning call**, which no other gate exercises. Generated code emits
  `dispatch(s, _t);` for a `jal`/`jalr` with no host return after it, so the caller's
  native frame is still live. It is an ordinary PASS.

`xcall` is also this gate's first worked example of a declared defect retiring itself.
It was added while the interpreter stopped only at a registered *entry*, so an
interpreted callee's `jr $ra` landed in the caller's interior and the caller's tail ran
twice — double-counting its work and reloading `$ra` from an already-popped frame. That
was recorded as a `CosimKnownDefect` naming exactly `r2` and `r31`.

A `CosimKnownDefect` is enforced in **both** directions: a declared field that stops
differing fails the gate just as loudly as an undeclared one that starts. When
[`rt: preserve AOT continuations across interpreter calls`][pr127] gave the interpreter
an explicit call boundary, this gate went red demanding the declaration be deleted —
which is what happened. Nothing had to remember to revisit it.

[pr127]: https://github.com/Jstar269/nakagawa-recomp/pull/127

Two details from that episode are worth keeping. `$sp` was deliberately *never* declared
even though it is also unwound twice: the generated entry epilogue restores it from
`_sp_entry`, which masks the leak, so naming it would itself have failed the gate. And
the declaration was written from measurement rather than expectation — the first draft
named `r29` and the comparator rejected it.

## Fail-closed negative corpus

The comparison above only ever exercises *successful* execution, so none of the
interpreter's refusals were covered: six safety guards could be deleted with every cell
still agreeing. These are **interpreter-tier** assertions rather than lane comparisons,
because lane `AOT` is deliberately more permissive — `MEM_*` absorbs an out-of-range
access through `sr_oor()` — and teaching it to manufacture matching faults purely to fit
a two-lane shape would be fabricating agreement.

Each case asserts the exact `SrGuestInterpResult`, the fault record, that no guest byte
in the observed window changed, and that architectural state changed in exactly the
declared ways. Alongside them:

* a **sweep** of all 14 control encodings, each placed in a delay slot, so the property
  asserted is "no control encoding is ever decoded as arithmetic" rather than one
  representative of it;
* the `jalr rd, rs` **link-register shape**, which the two-lane comparison structurally
  cannot reach (a callee entered through a link register other than `$ra` cannot return
  under the AOT frame model);
* an **interpreter form census** — see below.

Every negative program terminates even when the guard it tests is deleted. The
interpreter deliberately has no instruction cap, so a program that transfers back into
the scratch pad would turn a removed guard into a hang instead of a failure; transfers
name the registered trampoline, and a `break` sentinel bounds every straight-line run.
That is not hypothetical: the first version of the delay-slot case did hang under the
`skip-delay-slot` mutant.

## The form census

"Every form the interpreter implements exists because a cell executes it" used to be
enforced from one side only — a hand-maintained list describing the *fixture*. Nothing
checked the interpreter, so an opcode added with no cell behind it would have gone
unnoticed, which is exactly the speculative coverage the claim rules out.

The harness now asks the production decoder directly. Each candidate encoding is
executed as a real instruction; a form the interpreter does not decode fails closed as
`SR_GUEST_INTERP_UNSUPPORTED` *at that pc*, and anything else means it was decoded. The
decoded set must equal the set the cells execute, **in both directions**.

It earned its keep immediately: `mfc1`/`mtc1` dispatched on `fmt` alone, so every
reserved COP1 encoding sharing that `fmt` decoded as a register move. `src/rt/guest_interp.c`
now requires the defined-zero function field as well.

## The cells

Each cell is a self-contained guest function that relinquishes control through a
register transfer, so "the cell returned" is a deterministic synchronization point
in both lanes. Most end in `jr $ra`; `jrtail` ends in a computed tail call and
returns through its callee's `jr $ra` instead.

| cell | question |
| --- | --- |
| `alu` | three-operand and immediate ALU, all three shift kinds, signed vs unsigned compare |
| `r0` | `$r0` write suppression across the immediate, register and shift forms |
| `ldst` | word/half/byte traffic, sign vs zero extension, sub-word store lanes |
| `branch` | conditional branch delay-slot ownership, taken and not taken, condition read before the slot |
| `jump` | direct `j` with its slot, and a word only a mis-computed target could reach |
| `link` / `linkr` | `jal` and `jalr` link semantics with a conventional `$ra`-preserving frame |
| `jrslot` / `jrtail` | a computed call and a computed tail call whose delay slots rewrite the target register |
| `hilo` | `HI`/`LO` through signed and unsigned multiply (they differ in the high word only) |
| `fpu` | scalar FPU over the #120 helper path, re-run under all four FCR31 rounding modes and with FS set |
| `xcall` / `xtail` | **cross-tier** — a returning call and a tail transfer that each drop into the interpreter mid-run and hand back |
| `spleak` | **positive control** — an unbalanced `$sp` epilogue |

`spleak` is not a defect fixture. It pins the one architectural asymmetry the
lanes genuinely have: generated code closes every callable entry with
`s->r[29] = _sp_entry` on an o32 callee-saved-SP assumption, while the
interpreter executes only the instructions present. The cell declares that `r29`
must differ — and *nothing else* — so a comparator that stopped detecting it
fails, and so does one that started reporting extra fields.

### A constraint the fixture had to obey

Generated code models `jal`/`jalr` as a host **call**: the callee runs as a
nested C frame and its `jr $ra` is a host return. A computed transfer whose
target is *not* a registered function entry therefore returns into the middle of
an already-executing native body and double-executes it. Every computed transfer
in this fixture consequently targets a leaf that ends in `jr $ra`. A `jalr`
naming a link register other than `$ra` cannot be exercised coherently under that
model and is recorded as the next expansion rather than faked.

## What this proves, and what it does not

* Integer, memory and control-flow semantics are **independently implemented** in
  the two lanes, so a disagreement is real evidence.
* Scalar FPU arithmetic is **not** independently implemented: both lanes call the
  same `sr_fpu_*` helpers from [`src/rt/fp_convert.h`](../../src/rt/fp_convert.h).
  The `fpu` cell compares operand selection, register indexing and FCR31
  threading — not the arithmetic kernel, which
  [`src/rt/fp_convert_selftest.c`](../../src/rt/fp_convert_selftest.c) owns.
* Evidence tier **2 (production helper / white-box)**: the production dispatch
  core, the production interpreter and real codegen output all execute, but the
  `CpuState` seeding, the dispatch-table reset and the cell entry are
  test-specific.
* The interpreter is deliberately **more fail-closed** than the generated code on
  out-of-range and misaligned data access. The cells stay in range, so this never
  shows up as a divergence; it is a documented lane asymmetry, not a comparison.

## Proving the comparator is load-bearing

`mutate.py` applies one semantic defect at a time to a *copy* of the interpreter — or
of `tools/codegen.py`, via the `CODEGEN_TOOL` override — under the ignored build tree,
rebuilds the harness against it and requires the gate to fail. A mutant that only breaks
the build is reported `INVALID` and fails the campaign: a compile error proves the
compiler noticed, not the comparator.

Mutating both sides matters. A differential proven against one lane is half proven, so
one mutant restores the **generator** to its pre-fix computed-transfer emission order —
the exact production defect this fixture was built to find — and `jrslot`/`jrtail` must
both diverge.

Three interpreter guards are not killed by this gate, and each is layering rather than a
hole:

* deleting the `is_control_opcode()` guard inside `execute_noncontrol()` changes nothing
  observable, because every control encoding also lacks an arithmetic handler and fails
  closed through the default path. The sweep asserts the property; the guard becomes the
  load-bearing layer only once a primary opcode is implemented that a control encoding
  shares.
* `sr_exec_span_owns_fetch()` is the *dispatch* authority predicate, not the
  interpreter's fetch path — `src/rt/dispatch_isolation_selftest.c` owns it, and it does
  fail when that predicate is weakened (measured).

The campaign has already earned its keep: the `allow-r0-write` mutant **survived**
the first run, because the `r0` cell ended with a `nop` — which encodes as
`sll $zero, $zero, 0` and repaired `$r0` on the way out. No guest read can observe
`$r0` either, so the cell was silently vacuous.
`tools/test_cosim_fixture.py` now pins the fix.

## Files

| path | role |
| --- | --- |
| `generate.py` | the fixture recipe: guest module, relocations, and the generated C manifest of cell addresses |
| `cosim_selftest.c` | the comparator harness |
| `mutate.py` | the mutation campaign driver |
| `../../tools/test_cosim_fixture.py` | structural gates that need no toolchain |

No binary fixture is committed. `generate.py` is the source of truth; everything
it emits lands under the Git-ignored `build/` tree.

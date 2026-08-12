# Hardware oracle plan — a real PSP as an external verification source

**Status: proposal. The trace-oracle design below has not been built or tested.** Throughput figures
are estimates and labelled as such. This document is a plan in the same sense as
[DECOMPME_INTEGRATION.md](DECOMPME_INTEGRATION.md), not a record of completed work.

> **A narrower subset of this plan is now implemented.**
> The [source-owned scalar-probe runbook](../fixtures/psp_oracle/README.md), its
> [strict result protocol](../tools/psp_oracle/protocol.py), and the shipped
> [`tools/psp_readiness.py`](../tools/psp_readiness.py) cover the implemented subset. Read those
> first. In particular, the `tools/hw_doctor.py` proposed in §7 is superseded by the readiness tool
> — extend that tool rather than adding a second precondition checker.
> What remains genuinely unbuilt here is the *instruction-trace* oracle
> (`CODEGEN_ORACLE`/`MICROTEST_ORACLE` capture on real silicon), which the scalar probe does not
> provide.

## 1. The gap this closes

`tools/verify_gates.py` reports, verbatim:

```text
BLOCKED: CODEGEN_ORACLE not set (need a PPSSPP-captured .trace for <elf>)
```

**PPSSPP-captured** is the problem. Full verification currently compares this project against
another reimplementation. Where PPSSPP is wrong or approximate, we inherit the error invisibly.
[AGENTS.md](../AGENTS.md) already forbids describing renderer agreement as an external oracle; the
same limit applies to trace agreement.

A PSP running custom firmware executes on real Allegrex silicon and is the only ground truth
available in the absence of documentation.

## 2. Integration surface that already exists

| Existing piece | Role |
| --- | --- |
| [`TRACE_FORMAT.md`](../tools/TRACE_FORMAT.md) | Versioned textual CPU-state-diff format |
| `tools/gen_microtest.py` | Emits CRT-free Allegrex test modules; takes `--groups` / `--opcodes`; seeded and deterministic |
| `tools/microtest_gate.py` | Compares `src/ref` against an oracle trace, truncated at the first syscall so everything compared is pure CPU |
| `tools/codegen_gate.py` | Same shape, generated code vs oracle |
| `tools/verify_gates.py` | Orchestrates gates and reports BLOCKED rather than silently downgrading |
| `src/rt/vfpu_interp.c`, `vfpu_fuzz.c` | Existing differential harness over pinned tables |

The trace header is currently:

```text
# psp-recomp trace v1 target=<name> oracle=<ppsspp|interp|recomp> start_pc=<hpc> steps=<N>
```

Hardware support means extending that enum and recording provenance — see §9.

## 3. Architecture: a resident runner, not per-test rebuilds

Generating and cross-compiling a module per test costs a `psp-gcc` build and an upload per
iteration. Instead, build **one resident runner PRX that interprets test descriptors**:

```text
  Host                                     PSP (CFW + PSPLink)
  ----                                     -------------------
  write descriptor  ──> host0:in/NNN.bin ──> set state, execute, dcache writeback
  read + compare   <── host0:out/NNN.bin <── write result vector
  localize divergence, patch, repeat
```

The runner is built once. Each iteration is a file write, a device-side execution, and a file
read. This is the difference between a message pass and a build step, and it is what makes the
loop viable at all.

**`host0:` maps to the host directory `usbhostfs_pc` was started in.** That is the automation
primitive: no memory-stick swapping, no manual copying.

### Why not emit full per-instruction traces on hardware

Single-stepping via the PSPLink GDB stub would let the existing gates run unmodified, but it is
slow (est. tens to low hundreds of steps/sec) and does not scale.

**Recommended split:** result-vector comparison for bulk work (needs a small new
`tools/hwtest_gate.py`), and single-step traces only to localize a divergence once found. Bulk
compare to learn *that* something diverged; single-step to learn *where*.

## 4. Three loops, ranked

### Loop A — VFPU differential fuzz (highest value)

VFPU is under-documented, `assets/vfpu/` tables are PPSSPP-derived, and PPSSPP approximates parts
of it. Generate random (opcode, prefix, register state) → execute on hardware → compare against
`sr_vfpu_interp` → minimize → regression test. Fully mechanical, no game content, serves #36.
Target prefixes, NaN propagation, rounding modes, `vrot`, divide-by-zero, denormals.

### Loop B — Per-opcode microtests (lowest integration cost)

`gen_microtest.py` already emits the right module shape and already accepts `--opcodes`. Pointing
its oracle at hardware turns the microtest gate into a real gate.

### Loop C — Kernel/HLE semantics (largest issue payoff)

Where "PPSSPP does X" is explicitly not proof: #1 callbacks, #2 mutex/LwMutex, #13 semaphore
waiter cancellation, #14 async I/O, #16 FPL/thread-stack lifetime, #64 VBLANK sub-interrupts, #88
interrupt pending state. Bespoke per probe, but it is where the unanswered questions live.

## 5. Device choice: PSP vs PS Vita

A Vita does **not** contain Allegrex. It runs PSP titles through Sony's PspEmu on ARM. But
Adrenaline and ARK-4 do not reimplement the PSP kernel — they modify PspEmu to boot **real PSP
6.61 firmware modules**. So the Vita splits the two layers:

| Oracle | CPU | Kernel/HLE |
| --- | --- | --- |
| PSP | real silicon | real Sony firmware |
| Vita ePSP | emulated on ARM | **real Sony firmware** |
| PPSSPP | reimplemented | reimplemented |
| This project | reimplemented | reimplemented |

That split makes the disagreement pattern diagnostic:

| Pattern | Conclusion |
| --- | --- |
| PSP = Vita, PPSSPP differs | PPSSPP bug |
| PSP = PPSSPP, Vita differs | ePSP emulation artifact; discount the Vita for that class |
| PSP differs from both | Real silicon behaviour both emulators approximate — highest value |
| PSP ≠ Vita on kernel behaviour | Implicates the CPU-emulation layer or a firmware version gap |

**Calibrate before trusting.** Run the same microtest corpus on both devices. Where they agree, the
Vita is an empirically validated proxy *for that class*; where they diverge, the ePSP approximation
boundary is mapped. This replaces assumption with measurement, and the calibration corpus itself
becomes a committable regression asset.

**Do not use the Vita for instruction semantics.** Recording an emulation layer's approximations as
hardware truth would produce something that looks like tier-1 evidence and is not.

## 6. Physical setup

Requires a CFW-capable PSP (1000/2000/3000 use **mini-USB Type B**; the Go uses a proprietary
connector), a **Memory Stick PRO Duo**, a data-capable cable, and mains power.

1. **Firmware** — official 6.60 or 6.61 is a prerequisite for ARK-4.
2. **CFW** — install ARK-4 (`ARK_01234` → `PSP/SAVEDATA/`, `ARK_Loader` → `PSP/GAME/`), then make
   it permanent with Infinity so a reboot cannot silently drop the device out of CFW mid-session.
3. **PSPLink** — extract the psplinkusb release to `ms0:/PSP/GAME`.
4. **Windows driver** — with PSPLink running and USB connected, use Zadig: *Options → List All
   Devices*, select **`"PSP" type B`**, install the **`libusb-win32`** driver.
5. **Connect** — run `usbhostfs_pc` and `pspsh` in two terminals, **both opened in the build
   directory**. `pspsh` gives a `host0:/>` prompt.
6. **Toolchain** — PSPSDK on the host; build homebrew as an unencrypted `.prx` with `BUILD_PRX=1`.
   Keep PSPSDK out of the native build, like the other optional tools in [SETUP.md](SETUP.md).

**Vita transport (if used):** Adrenaline exposes `ux0:/pspemu/` as the ePSP's virtual memory stick,
so `ms0:` ↔ `ux0:pspemu/`, and VitaShell serves FTP on port 1337. That routes around USB
device-mode entirely. Caveat: VitaShell's FTP needs VitaShell in the foreground while Adrenaline
takes the foreground when running, making this a batch loop rather than a tight one.

### First milestone

A hello-world `.prx` built on the host runs on the device without touching the memory stick, and
something it writes appears in the host build directory. If that is painful, reconsider before
building further.

## 7. Determinism rules

Violating these makes the oracle lie:

- **Set `fcr31` explicitly per test.** Allegrex flush-to-zero and denormal behaviour depends on it,
  and that is often exactly what is being measured.
- **Keep the measured window single-threaded / interrupts disabled.**
- **`sceKernelDcacheWritebackAll`** (or the range variant) before the host reads a result buffer,
  or stale bytes are read over USB.
- **Pin the CPU clock** (222 vs 333 MHz) for anything timing-sensitive.
- **Record model, firmware, CFW version and clock** in every trace header.

## 8. Agent execution contract

An AI agent may own the host-side work. It must **never**:

- install or flash custom firmware, or update console firmware;
- install USB drivers (the Zadig step);
- handle retail game content in any form.

Physical gates requiring a human: inserting the memory stick, firmware/CFW install, the Zadig
driver step, launching PSPLink, connecting USB, power-cycling after a hang, and Vita foreground
app switching.

An agent may own: the PSPSDK toolchain, the runner PRX and its build, the descriptor format and
codecs, `tools/hwtest_gate.py`, driving `pspsh` non-interactively, comparison, divergence
bisection, fuzz-input minimization, and writing regression tests.

**The handoff must be machine-checkable.** This is now partly shipped as
`tools/psp_readiness.py`, which verifies `psp-gcc`/`usbhostfs_pc`/`pspsh` presence and reports
`OPTIONAL_MISSING`/`HARDWARE_NOT_CONNECTED` rather than silently downgrading. Still outstanding is
the *real* precondition: that a file round-trips through `host0:` in both directions, and that
`usbhostfs_pc` is actually running and `pspsh` responds. Extend `tools/psp_readiness.py` with those
live checks; do not add a separate `tools/hw_doctor.py`.

Agents without a persistent background process cannot host `usbhostfs_pc` and cannot drive the USB
loop; route those to the batch path.

## 9. Repository changes required

1. `tools/TRACE_FORMAT.md` — extend `oracle=` with `hardware-psp` and `vita-epsp`; add
   model/firmware/CFW/clock fields.
2. `tools/hwtest_gate.py` — **new**, result-vector comparison.
3. `tools/psp_runner/` — **new**, PSPSDK sources for the resident runner, excluded from the native
   build.
4. ~~`tools/hw_doctor.py` — **new**, the machine-checkable precondition check.~~ Superseded by the
   shipped `tools/psp_readiness.py`; add the live `host0:` round-trip check there.
5. `Makefile` — a `hw-verify` target reporting BLOCKED when no device is attached.
6. `tools/verify_gates.py` — register the hardware gate with the same BLOCKED semantics.
7. `publish_audit.py` / `.gitignore` — permit homebrew traces, reject retail-derived captures.

## 10. Scope discipline

| Artifact | Status |
| --- | --- |
| Homebrew module authored here + its hardware trace | Ours. Committable. |
| VFPU fuzz corpus + hardware results | Ours. Committable. |
| Any trace of the **retail game** on hardware | Game-derived. Private, permanently. |
| Frame or memory dumps from the retail title | Game-derived. Private, permanently. |

This is the strategic argument for the whole plan. A pre-republication tracker
item, historically numbered #35, was blocked on obtaining oracle evidence
without redistributing proprietary inputs. **Hardware microtests authored here are the
only oracle path identified so far that is committable**, and therefore the only one that could
ever run in public CI. Loops A and B stay entirely on the committable side; Loop C does too, as
long as probes are homebrew rather than instrumentation of the shipped title.

## 11. Limits and anti-patterns

- **Not a decompilation aid.** Recovering source that compiles to identical bytes is a
  compiler-output matching problem ([DECOMPME_INTEGRATION.md](DECOMPME_INTEGRATION.md)). Hardware
  has nothing to say about it. This is behavioural verification only.
- **Never majority-vote oracles.** With several oracles it is tempting to take best-of-N. Two
  emulators can outvote real silicon, which is exactly how an emulation artifact becomes enshrined
  as ground truth. Disagreement is data.
- **Never merge provenance.** A Vita result must never be readable as a PSP result.
- **One console is one data point.** Model and firmware differences exist; do not generalize.
- **A hardware result proves the behaviour actually executed** — one opcode, not a subsystem.
- **All oracles agreeing does not mean correct.** They may share an assumption inherited from the
  same documentation.
- **Crash recovery needs a human** or a USB-controlled relay; a hard hang requires a power cycle.
- **A whole-game hardware trace is not realistic.** Do not plan around it.

## Sources

- [PSPLink Windows setup](https://pspdev.github.io/psplink/windows.html) ·
  [PSPLink debugging](https://pspdev.github.io/debugging.html) ·
  [pspdev/psplinkusb](https://github.com/pspdev/psplinkusb) ·
  [PSPSDK](https://github.com/pspdev/pspsdk)
- [ARK-4](https://github.com/PSP-Archive/ARK-4) ·
  [Installing ARK-4](https://consolemods.org/wiki/PSP:Installing_ARK-4_CFW) ·
  [ARK-4 Infinity](https://github.com/PSP-Archive/ARK-4/wiki/Infinity)
- [Adrenaline](https://github.com/TheOfficialFloW/Adrenaline) ·
  [Adrenaline (ConsoleMods)](https://consolemods.org/wiki/Vita:Adrenaline) ·
  [VitaShell FTP](https://consolemods.org/wiki/Vita:Transferring_Files_with_FTP)

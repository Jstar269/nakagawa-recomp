# VFPU oracle baseline — 2026-08-05

**RESOLVED the same day by a hardware capture.** See "Hardware verdict" below;
the software-only sections that follow are retained as the record of how the
question was posed.

## Method

One probe binary, one shared input vector (`fixtures/vfpu_oracle/vfpu_oracle_cases.h`,
46 raw IEEE-754 bit patterns), digested per operation with FNV-1a over the raw
result bits.

- **PPSSPP side** — the probe PRX executed under PPSSPP headless. Emits
  `source=ppsspp`, so the comparator's role check correctly refuses it as hardware
  evidence.
- **Nakagawa side** — `mingw32-make psp-oracle-vfpu`, driving the production
  `sr_vfpu_*` in `src/rt/recomp.c` over the same vector.

Both sides report `out3 = 0x2e` (46 inputs), confirming they ran the same vector.

## Result: the two implementations disagree on six of eight operations

| operation | PPSSPP | Nakagawa | |
| --- | --- | --- | --- |
| `vasin` | `0x7e39e3d2` | `0x7e39e3d2` | agree |
| `vsin` | `0x0235f855` | `0x0235f855` | agree |
| `vrcp` | `0xcc94d44f` | `0xe1ec308a` | **differ** |
| `vrsq` | `0x4626b431` | `0x657aaf94` | **differ** |
| `vsqrt` | `0x91633367` | `0x0e1bce1b` | **differ** |
| `vlog2` | `0x3a33e093` | `0x8bff5b53` | **differ** |
| `vcos` | `0x8124b5bd` | `0x2e4b69bd` | **differ** |
| `vexp2` | `0xc7598f46` | `0x6558a646` | **differ** |

Spot values localize part of it. Indices are into the shared vector:

| input | operation | PPSSPP | Nakagawa |
| --- | --- | --- | --- |
| `0x3F800001` (1.0 + 1 ulp) | `vrcp` | `0x3f7ffffe` | `0x3f7ffffc` |
| `0x00000001` (min denormal) | `vrsq` | `0x64b504f3` | `0x7f800000` (+inf) |
| `0x00000001` (min denormal) | `vsqrt` | `0x1a3504f3` | `0x00000000` |

## What this does and does not establish

**Established.** The divergence is real and reproducible, and it is not a missing
table: `sr_load_raw` calls `abort()` with a diagnostic when a table is absent or
short, and the harness ran to completion. The two implementations genuinely
compute different results for the same inputs.

**Not established.** Which one is right, and why they differ. Two candidate
causes, not yet separated:

1. **Denormal handling.** Nakagawa appears to flush: `vsqrt(denormal) -> 0`,
   `vrsq(denormal) -> +inf`, where PPSSPP returns finite values. Many embedded
   FPUs do flush, so either could be faithful.
2. **Host floating-point environment.** The harness passes `float` through a
   normal C call. If the host build has FTZ/DAZ semantics in play, a denormal
   could be flushed before `sr_vfpu_*` ever sees it — which would make part of
   this an artifact of the harness rather than of the runtime.

Cause 2 does **not** explain everything: `vrcp(1.0 + 1 ulp)` differs by 2 ulp and
involves no denormal at all. So at least some of the divergence is a genuine
implementation difference. Separating the two is the first follow-up, and it is
cheap — have the harness echo the bits it actually received.

## Why the existing fuzzer never saw this

`vfpu_fuzz` compares generated code against `sr_vfpu_interp`. Both route through
the same tables and the same range reduction, so they agree by construction. It
is a codegen test, not an accuracy test, and no amount of running it can surface
a divergence of this kind.

## Next

1. Rule cause 2 in or out by echoing received bits in the harness.
2. Capture the probe on the PSP-3001. Whichever side hardware agrees with is
   correct; the other is a defect. Note that the PSP side is currently the
   *only* one of the three that has never been measured.
3. `vcos` deserves its own attention: its three spot values agree with PPSSPP
   while the digest differs, so its divergence lies in an input not covered by
   the spots — most likely the large-argument Group B/C region.

Recorded as measurement only. No claim is made about correctness of either side.

---

## Hardware verdict (PSP-3001, 6.61-ARK, PSPLINK)

The probe was captured on real Allegrex. **Hardware matches Nakagawa on all eight
digests, exactly.**

| operation | hardware | Nakagawa | PPSSPP (interp) | PPSSPP (JIT) |
| --- | --- | --- | --- | --- |
| `vrcp` | `0xe1ec308a` | **match** | `0xe1ec308a` match | `0xcc94d44f` differ |
| `vrsq` | `0x657aaf94` | **match** | `0x4626b431` differ | `0x4626b431` differ |
| `vsqrt` | `0x0e1bce1b` | **match** | `0x91633367` differ | `0x91633367` differ |
| `vasin` | `0x7e39e3d2` | **match** | match | match |
| `vlog2` | `0x8bff5b53` | **match** | `0x3a33e093` differ | `0x3a33e093` differ |
| `vsin` | `0x0235f855` | **match** | match | match |
| `vcos` | `0x2e4b69bd` | **match** | `0x8124b5bd` differ | `0x8124b5bd` differ |
| `vexp2` | `0x6558a646` | **match** | `0xc7598f46` differ | `0xc7598f46` differ |

### The denormal question is answered

`sr_vfpu_sqrt` and `sr_vfpu_rsqrt` flush denormals on their first line —
`if((bits&0x7FFFFFFFu)<=0x007FFFFFu)` returns `+0.0` and `±inf` respectively.
PPSSPP instead computes them (`0x1a3504f3` is the true 2^-74.5).

Hardware returns `vsqrt(min denormal) = 0x00000000` and
`vrsq(min denormal) = 0x7f800000`. **Nakagawa's flush is correct silicon
behaviour; PPSSPP's computed value is not.** The runtime needs no change here.

### Controls run before drawing that conclusion

- **Host FP environment** — a standalone test confirmed denormals survive the
  exact harness path (`memcpy` -> float -> call -> `memcpy`) intact at both `-O0`
  and `-O2`, including through a multiply. Not an FTZ/DAZ artifact.
- **Same tables** — all fifteen `.dat` files under `third_party/ppsspp-src/assets/vfpu/`
  are byte-identical to this repository's pinned copies.
- **Tables demonstrably load** — `load_vfpu_table` leaves the pointer `nullptr` on
  failure and the compute paths dereference without a null check, so a failed load
  crashes rather than degrading. Headless produced values, and `vsin`/`vasin`
  match hardware exactly.
- **Working directory** — identical results from three different CWDs including
  the installed PPSSPP tree, so asset resolution is not the variable.
- **Both PPSSPP cores** — JIT and interpreter agree on five of the six
  divergences; only `vrcp` differs between them, where the *interpreter* matches
  hardware and the JIT does not.

### What this establishes, and what it does not

Nakagawa's VFPU transcendentals are hardware-correct on this input vector. That
retires the largest inherited-assumption risk in the runtime: `assets/vfpu/` is
PPSSPP-derived, but the implementation around it now has silicon backing.

It does **not** establish a defect in released PPSSPP. This was one locally-built
headless binary from a source checkout of unknown configuration, exercised over
46 inputs. The divergences are reproducible and the controls above rule out the
obvious environmental causes, but confirming them against a release build is a
prerequisite before reporting anything upstream.

The `vrcp` JIT-versus-interpreter split is the most interesting single result and
the easiest to act on, since it is internal to PPSSPP and does not depend on
hardware access to reproduce.

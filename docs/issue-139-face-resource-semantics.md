<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# `face/00/100_f_face*.gim` — resource semantics root cause (#139, #196)

Static analysis, 2026-08-03. Base `origin/main` = `b31f5aa`.

**Headline: `00` is a literal directory produced by `%02d` of an integer, not a mount slot.**
The mixed path is built entirely by guest code from guest asset content. #196's mounted-slot
hypothesis is resolved *negative* for this path, and no host-side alias, basename fallback, or
try-all-archives behaviour is justified.

## Evidence discipline

Every claim below is tagged. Do not promote a tag when citing this document.

| Tag | Meaning |
| --- | --- |
| **[P]** | **Proven.** Directly readable from the guest image at the cited address, or from a deterministic property of the cited format string. Re-derivable without the game running. |
| **[I]** | **Strong inference.** Follows from **[P]** facts plus one modest assumption that is stated inline. |
| **[H]** | **Hypothesis.** Consistent with the evidence, not established. Needs a bounded probe. |

This is source-shape / static evidence (tier 4 on the `AGENTS.md` ladder). It is *decisive* about
how the path is constructed, because format strings and asset-embedded name tables are the ground
truth for string construction. It is **not** production-dispatch evidence about what the game does
at runtime, and it does not by itself establish the cause of the #139 visual defect.

## Method

Read-only static analysis of the flat guest image (`GAME_BASE=0`, so guest address equals file
offset), plus structural inspection of the locally extracted archive tree. No runtime probe, no
route replay, no runtime change. Analysis scripts were throwaway and live outside the tree; the
findings are re-derivable from the cited addresses. Bulk scan output over retail content is
deliberately **not** reproduced here — see "Publication boundary" below.

## The two path components have independent origins

The failing request is `data/chara/model/face/00/100_f_face{0,1,2}.gim`. The directory and the
filename come from unrelated sources. Every previous pass treated the request as one token, which
is why it kept stalling at the "formatter buffer already contains `face/00`" boundary.

### Directory `face/00/` — `%02d` of an integer **[P]**

At `0x002a85b4`–`0x002a85c4` in `f_002a805c`, the guest formats
`"data/chara/model/face/%02d/"` (constant at `0x00301dec`) with register `s3`.

`%02d` pads but does not truncate, so `100 -> "100"`, `102 -> "102"`, `0 -> "00"`. Observing
`face/00` therefore proves `s3 == 0` exactly. Five further copies of the same format constant
exist (`0x002d29bc`, `0x002d4548`, `0x002e06fc`, `0x002ef688`, `0x002fce88`); all are `%02d` of a
plain integer. **No code path treats the directory component as a string, a slot id, or an archive
handle.**

`s3` is loaded at `0x002a83f4`–`0x002a83f8` as `*(t1_arg)`; the wrapper `f_002a7fa8` fills that
array from `(a0->[0x10])->[0x1C]`, the scorecard's character id for slot 0. The same value reaches
the callee as stack argument 11 and drives the `(id < 15) ? id : 99` clamp at `0x002a8400`.

### Filename `100_f_face{0,1,2}` — asset-embedded, not code-built **[P]**

The filename stem is not constructed by code. It is the texture-name list embedded in the shared
rig models that `f_002044b4` selects for part indices 2..5:

| part idx (`f_00204258`) | model chosen by `f_002044b4` | face texture names it declares |
| --- | --- | --- |
| 0, 1 | `menu_motion/<id>/chrm_pc<id>_0{0,1}.tat` | the character's own |
| 2, 3 | `menu_motion/99/chrm_pc_m_0{0,1}.tat` | `100_f_face{0,1,2}` |
| 4, 5 | `menu_motion/99/chrm_pc_f_0{0,1}.tat` | `100_f_face{0,1,2}` |

The union of face texture names across those four shared-rig models is exactly
`{100_f_face0, 100_f_face1, 100_f_face2}` — **exactly three**, matching the exactly three observed
`sceIoOpen` misses. `f_0004f6b4` keys its texture cache on the bare name only (`strcmp` at
`0x0004f738`; the directory prefix is not part of the key), so four models collapse to three opens
even when more than one portrait is loaded.

## The rename mechanism the engine does have **[P]**

`100_f_` is a *placeholder* prefix. The engine has exactly one mechanism to rewrite it:

1. `f_00092e50` reads a character id from `(a0->[0x50])->[0x1B4]` (word) and a variant index from
   `->[0x1B8]` (halfword), resolves the record through `f_000d8c24`, and at
   `0x00092ec0`–`0x00092ecc` sets the source prefix to the `"100_f_"` constant (`0x002d29b4`)
   **only when its `t1` argument is non-zero**.
2. `f_00093094` formats the destination prefix `"%03d_%c_"` (`0x002d2a44`) from that id and a
   four-entry character-code table at `0x002beb34`.
3. `f_0005d100` performs the substitution at `0x0005d18c`–`0x0005d1e0`: `strstr(name, src)`, copy
   the head, append the destination prefix, append the tail — guarded by `src != 0 && dst != 0`.

Call chain:
`f_00092e50 -> f_0005ec14 -> f_0005d638 -> f_0005d8d0 -> f_0005d100 -> f_0004f6b4 -> f_0004cad0
-> f_0004ccb8 -> sceIoOpen`. This is consistent with the return address `0x0004f884` recorded by
the earlier runtime traces on #196.

The destination prefixes the table can produce line up exactly with the naming convention of the
higher-numbered face banks, and not at all with the low-numbered ones.

## The defect: the scorecard alone disarms the rename **[P]**

`t1` at all ten `f_00092e50` call sites:

| call site | owning function | `t1` |
| --- | --- | --- |
| `0x00098728` | `f_00097520` | `s6` (variable) |
| `0x0017bbac` | `f_0017b6b0` (character select) | `f_0020452c(idx) & 0xff` |
| `0x00205274` | `f_0020507c` | `s4->[108]` |
| `0x00253fd4`, `0x00253ff8` | `f_00253f38` | `1`, `a1` |
| `0x00254020`, `0x00254044` | `f_00253f38` | `1`, `a1` |
| `0x002540b4`, `0x002540e8` | `f_00253f38` | `1`, `a1` |
| **`0x002a85dc`** | **`f_002a805c` (scorecard)** | **`0` — hardwired at `0x002a85e0`** |

`f_0020452c(idx)` returns `1` for exactly `idx ∈ {2,3,4,5}` — precisely the set for which
`f_002044b4` substitutes the shared `menu_motion/99` rig. It *is* the "this part uses the shared
rig, so rewrite its placeholder texture names" predicate. The character-select path passes it. The
scorecard passes a hardcoded zero, so `100_f_face*` survives verbatim and is concatenated with
`face/<scorecard id>/`.

`f_002a805c` is identified as the scorecard by its own adjacent string pool near `0x00301dec`,
which contains the set/game score-field and portrait-slot element names for that overlay.

## "Character 100" is a red herring **[P]**

The failing case is **not character 100**. `100_f_` is a placeholder token; the character being
drawn has id **0**. Any reading of the request as "character 100 under directory 00" is chasing a
number that no selector ever produced.

The working `face/102/102_f_face{0,1,2}.gim` requests come from a call site that **arms** the
rename: `100_f_` is rewritten to `102_f_` while the directory is `%02d` of 102, so both halves
agree. The difference between the failing and working case is the *call site*, not the character.

## Archive layout: mounted-slot hypothesis falsified **[P]** / **[I]**

A bounded structural scan of the extracted archive tree compared archive-internal paths across all
archives. Result for the subtree that matters:

- **[P]** Every colliding inner path under `data/chara/model/face/` is a byte-size-identical
  duplicate of the same asset packed into more than one bundle archive. There are **no**
  differing-content collisions anywhere under that subtree.
- **[P]** The handful of differing-content collisions found tree-wide are in unrelated subsystems
  and do not involve character face resources.
- **[I]** Therefore a flattened index is lossless for face resources, and `host_data_lookup()`
  is not discarding any archive-precedence or slot indirection on this path. The assumption is
  only that the extracted tree faithfully represents the packed archives, which the earlier direct
  packed-entry probe on #196 already checked for these specific keys.

Directory numbering corroborates that `00` is an ordinary bank rather than a slot: the
`menu_motion` directory set is exactly the low character ids plus the shared `99` rig — the same
set the `(id < 15) ? id : 99` clamp at `0x002a8400` encodes — while the `face` tree additionally
carries higher-numbered banks. The low banks and the high banks use two different, non-overlapping
texture-naming conventions, and only the high-bank convention is producible by the `"%03d_%c_"`
formatter.

## Consequence: the three misses are very likely retail-faithful **[I]**

- **[P]** The `t1 = 0` at `0x002a85e0`, the format constant at `0x00301dec`, and the shared rig's
  embedded name table are all retail data. The recompiler does not synthesise any of them.
- **[P]** No archive exports `data/chara/model/face/00/100_f_face0.gim` under any key.
- **[P]** The rename's output for a low-numbered character id would also not exist, because the low
  banks use a different naming convention entirely. So arming the rename would not fix this case.
- **[I]** Retail therefore constructs the identical request and finds nothing. The assumption is
  that retail reaches the same branch with the same state, which the recompiled run's own trace
  already demonstrates for the recompiled binary.

**No `00 -> 100` alias, basename search, or try-all-archives fallback can be correct**: the
entries do not exist under any key in any archive, so there is nothing to alias *to*. This is
exactly the class of fix #32 and the project's no-band-aid rule exclude.

## What this means for #139 and #196

- **#196 — resolved negative [P]/[I].** `00` is a literal directory produced by `%02d` of a
  character id. There is no mount/select lifecycle missing on this path, and archive flattening is
  lossless for face resources. The remaining #196 scope (direct archive VFS, extraction removal,
  fixture suite) is unaffected, but it should no longer be blocked on the face-path question.
- **#139 — reopened at a different layer [I]/[H].** The three misses are very likely not the cause
  of the empty portrait circles, because retail almost certainly hits them too. The visual defect
  needs a separate cause, and #139 should be re-scoped away from the file layer.

## Remaining #139 hypothesis **[H]**

> The empty circles are caused by the scorecard's portrait model never loading or never being
> rendered — not by the three face-texture misses. The most likely single point of failure is the
> guard byte tested at `0x002a83ec`, which gates the entire portrait-loading loop; if it is zero,
> no portrait model is loaded at all.

Bounded probe to settle it, in order, without touching the VFS:

1. At `0x002a83f8` record `s3` and the guard byte tested at `0x002a83ec`. A zero guard means the
   loop never runs — that, not the texture misses, would produce empty circles.
2. Record whether the **idx 0/1** opens succeed: `menu_motion/<s3>/chrm_pc<s3>_0{0,1}.tat` and the
   character's own face textures. Only these carry the actual portrait. Note that the portrait loop
   uses the **unclamped** `s3` while the neighbouring motion loop uses the clamped `s2`
   (`0x002a8400`–`0x002a840c`), so for any character id >= 15 the portrait loop requests a
   `menu_motion/<id>/` directory that does not exist — a second, independent failure mode worth
   capturing in the same probe.
3. Only if idx 0/1 resolve cleanly and the model still does not appear should the investigation
   move to the render path (`f_0005743c`, `f_000575fc`, and the portrait layout element).

## Publication boundary

This document deliberately records only what is needed to act on the finding: guest addresses,
static control flow, structural conclusions, and the small number of interoperability constants
(format strings and the three failing filenames, already recorded in #139) without which the
argument cannot be checked. It contains no retail bytes, no asset inventories or directory
listings, no extracted-tree paths from the local machine, no disassembly dumps, and no hashes of
private inputs. The full scan output and the analysis scripts remain local and untracked.

## Address index

| Address | Role |
| --- | --- |
| `0x002a7fa8` | scorecard wrapper; fills the slot-id array from `(a0->[0x10])->[0x1C..0x28]` |
| `0x002a805c` | scorecard portrait loader (`f_002a805c`) |
| `0x002a83ec` | guard gating the whole portrait loop — the #139 probe point |
| `0x002a83f8` | `s3 = *(t1_arg)` — the directory character id |
| `0x002a8400` | `(id < 15) ? id : 99` clamp; applied to the motion loop only |
| `0x002a85bc` | formats `"data/chara/model/face/%02d/"` with `s3` |
| `0x002a85e0` | **`t1 = 0`** — rename disarmed, unique to this call site |
| `0x00092e50` | `f_00092e50`; reads char id `(a0->[0x50])->[0x1B4]`, variant `->[0x1B8]` |
| `0x00092ec0` | `if (t1 != 0) srcPrefix = "100_f_"` |
| `0x00093094` | builds the `"%03d_f_"` / `"%03d_%c_"` destination prefixes |
| `0x000d8c24` | record lookup: `bank = db + kind*12`, `rec = bank[4][idx]`, invalid if `rec[0x60] == -1` |
| `0x0005d100` | `strstr` + splice prefix substitution |
| `0x0004f6b4` | texture load; `sprintf(buf, "%s%s.gim", dirPrefix, name)` at `0x0004f7a4` |
| `0x0004f738` | name-only texture cache compare; prefix is not part of the key |
| `0x00204258` | part-name format table for the six portrait parts |
| `0x0020443c` | motion path builder; substitutes `99` for idx 2..5 |
| `0x002044b4` | model path builder; substitutes `99` for idx 2..5 |
| `0x0020452c` | "uses the shared 99 rig" predicate: `1` for idx 2..5 |
| `0x002beb34` | four-entry character-code table used by `"%03d_%c_"` |
| `0x002d29b4` | `"100_f_"` placeholder source prefix |
| `0x002d2a44` | `"%03d_%c_"` destination prefix format |
| `0x00301dec` | scorecard's `"data/chara/model/face/%02d/"` |

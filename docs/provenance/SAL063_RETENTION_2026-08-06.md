# sal063 retention measurement — 2026-08-06 (IND-4)

Resolves finding **PROV-F2**, which recorded that the fifteen `Derived from
sal063/PSP-recompilation-project` headers were unverifiable and *might be boilerplate*.

**They are not boilerplate. They are accurate, and they under-state the inheritance.**

This is a measurement record. It changes no code and removes no attribution. Conclusions that
require a PPSSPP source comparison are marked as still open.

## Method

Upstream: `sal063/PSP-recompilation-project`, public clone, `da17b0e` (2026-06-15). Nakagawa's own
first commit is 2026-07-19, so the fork direction is upstream → Nakagawa, consistent with
`NOTICE.md`.

For each file present in both trees, blank lines and comments are stripped, whitespace is collapsed,
and `difflib.SequenceMatcher` measures the longest common subsequence of code lines. Two ratios are
reported because they answer different questions:

- **`up_ret%`** — how much of *sal063's* code is still here. Answers "how much did we inherit?"
- **`cur_of%`** — how much of *the current file* came from sal063. Answers "how much of what we ship
  is theirs?"

### Measurement limits — read before quoting a number

- `#` lines are treated as comments in `.py` and as **preprocessor directives in C/C++**. An earlier
  pass stripped them everywhere and under-measured headers; that is corrected here.
- Longest-common-subsequence over normalized lines credits a shared line wherever it appears. Short
  or repetitive lines (`}`, `return 0;`) inflate the count slightly. Treat these as **close upper
  bounds**, not exact figures.
- Similarity is not a legal conclusion. It measures textual retention, not what is protectable.
- Only files present in **both** trees are compared. Nakagawa has 536 tracked files; 44 sources are
  common.

## Headline

| | |
| --- | --- |
| common source files | **44** |
| sal063 normalized code lines | 13,482 |
| still present in Nakagawa | **11,310 — 83.9% of upstream** |
| current normalized lines in those 44 files | 28,188 |
| share of that which is shared with sal063 | **40.1%** |

Nakagawa has roughly doubled these files while retaining the overwhelming majority of what it
started from.

## Per-file

| file | up_ln | cur_ln | shared | up_ret% | cur_of% | declares |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `src/rt/osk_win.c` | 69 | 69 | 69 | 100.0% | 100.0% | sal063 |
| `tools/funcdiff_cmp.py` | 37 | 37 | 37 | 100.0% | 100.0% | — |
| `tools/ppm2png.py` | 22 | 21 | 21 | 95.5% | 100.0% | — |
| `src/rt/nid_names.h` | 1638 | 1634 | 1630 | 99.5% | 99.8% | — |
| `tools/tracediff.py` | 81 | 81 | 78 | 96.3% | 96.3% | — |
| `src/ref/interp.h` | 30 | 32 | 30 | 100.0% | 93.8% | sal063 |
| `src/ref/cpu.h` | 67 | 79 | 65 | 97.0% | 82.3% | sal063 |
| `src/rt/mpeg.c` | 439 | 494 | 403 | 91.8% | 81.6% | PPSSPP |
| `src/rt/gpu_sdl3vk/sdl3vk.h` | 22 | 27 | 22 | 100.0% | 81.5% | — |
| `src/rt/h264_mf.c` | 349 | 425 | 313 | 89.7% | 73.6% | sal063 |
| `src/rt/funcdiff.c` | 87 | 107 | 77 | 88.5% | 72.0% | sal063 |
| `src/rt/pgf.c` | 298 | 395 | 277 | 93.0% | 70.1% | PPSSPP |
| `tools/nidseq.py` | 43 | 50 | 35 | 81.4% | 70.0% | — |
| `src/ref/interp.cpp` | 362 | 482 | 333 | 92.0% | 69.1% | sal063 |
| `src/rt/vfpu_interp.c` | 373 | 543 | 344 | 92.2% | 63.4% | PPSSPP |
| `src/rt/ge.c` | 1962 | 2957 | 1829 | 93.2% | 61.9% | PPSSPP |
| `src/rt/savedata.c` | 525 | 662 | 403 | 76.8% | 60.9% | PPSSPP |
| `src/rt/gui.c` | 263 | 255 | 132 | 50.2% | 51.8% | sal063 |
| `src/rt/gpu_sdl3vk/sdl3vk.c` | 410 | 640 | 322 | 78.5% | 50.3% | PPSSPP |
| `src/rt/ge_shared.h` | 68 | 147 | 68 | 100.0% | 46.3% | sal063 |
| `src/rt/vfpu_fuzz.c` | 108 | 225 | 104 | 96.3% | 46.2% | sal063 |
| `src/ref/run_elf.cpp` | 110 | 211 | 97 | 88.2% | 46.0% | sal063 |
| `src/rt/iso.h` | 7 | 16 | 7 | 100.0% | 43.8% | sal063 |
| `tools/analyze.py` | 325 | 641 | 280 | 86.2% | 43.7% | — |
| `tools/microtest_gate.py` | 53 | 101 | 39 | 73.6% | 38.6% | — |
| `tools/codegen_gate.py` | 65 | 126 | 48 | 73.8% | 38.1% | — |
| `tools/codegen.py` | 735 | 1644 | 617 | 83.9% | 37.5% | — |
| `src/rt/gpu_sdl3vk/ge_gpu.c` | 1255 | 2791 | 1008 | 80.3% | 36.1% | PPSSPP |
| `tools/gen_microtest.py` | 116 | 252 | 91 | 78.4% | 36.1% | — |
| `src/rt/audio.c` | 100 | 110 | 39 | 39.0% | 35.5% | sal063 |
| `src/ref/selftest.cpp` | 188 | 534 | 170 | 90.4% | 31.8% | sal063 |
| `src/rt/driver.c` | 132 | 340 | 104 | 78.8% | 30.6% | sal063 |
| `tools/prxload.py` | 110 | 346 | 81 | 73.6% | 23.4% | — |
| `src/rt/recomp.h` | 123 | 444 | 103 | 83.7% | 23.2% | PPSSPP |
| `src/rt/hle.c` | 1627 | 6431 | 1282 | 78.8% | 19.9% | PPSSPP |
| `src/rt/sched.c` | 475 | 1914 | 379 | 79.8% | 19.8% | sal063 |
| `tools/imports.py` | 66 | 237 | 47 | 71.2% | 19.8% | — |
| `tools/vfpu_fuzz_gen.py` | 57 | 156 | 28 | 49.1% | 17.9% | — |
| `tools/ppmdiff.py` | 43 | 159 | 25 | 58.1% | 15.7% | — |
| `src/rt/recomp.c` | 427 | 1680 | 214 | 50.1% | 12.7% | PPSSPP |
| `src/rt/gpu_sdl3vk/ge_gpu.h` | 13 | 122 | 13 | 100.0% | 10.7% | — |
| `src/rt/iso.c` | 133 | 380 | 32 | 24.1% | 8.4% | — |
| `tools/gen_nidnames.py` | 59 | 187 | 11 | 18.6% | 5.9% | — |
| `src/rt/pgf.h` | 10 | 4 | 3 | 30.0% | 75.0% | PPSSPP |

## What this establishes

### 1. PROV-F2 is resolved, and its hypothesis was wrong

The prior audit speculated that some sal063 headers looked like a template — specifically that
`h264_mf.c` "drives Windows Media Foundation" and so had "no plausible counterpart in a PSP
recompilation toolkit", and that `sched.c`'s architecture was project work implying "low retention".

Both were wrong. `h264_mf.c` retains **89.7%** of an upstream file that already existed, and
`sched.c` retains **79.8%**. `osk_win.c`, `iso.h`, `ge_shared.h` and `interp.h` are at **100%**.

Keeping the attribution was the right call; the stated reason for doubting it was not. Recorded here
rather than quietly deleted, because a provenance audit that hides its own corrections is worth less
than one that shows them.

### 2. The PPSSPP attribution is Nakagawa's own work, not inherited

Of sal063's 44 tracked sources, exactly **one** carries a `Derived from` header: `ge_gpu.c`, which
credits PPSSPP. Every Nakagawa file whose header declares PPSSPP derivation — `ge.c`, `hle.c`,
`recomp.c/.h`, `savedata.c`, `mpeg.c`, `pgf.c/.h`, `vfpu_interp.c`, `evf.h` — **acquired that header
in Nakagawa**.

The chain is PPSSPP → sal063 (largely undeclared at file level) → Nakagawa (declared). Nakagawa's
per-file attribution is *more* complete than its immediate upstream's, which is a point in the
project's favour and should be stated that way.

### 3. `ge_shared.h` is a chain, not a contradiction

PROV-F1 flagged that its header says sal063 while `NOTICE.md` says PPSSPP `Core/GE`. Both are true
at different levels: 100% of it comes from sal063, and sal063's GE state structures are themselves
PPSSPP-derived. The defect is an under-specified chain, not a conflict. The fix is to record both
links, not to choose one.

### 4. `audio.c` — header confirmed, NOTICE claim still unverified

39.0% of sal063's `audio.c` is retained, so the sal063 header is correct. But sal063's own
`CREDITS.md` itemizes its PPSSPP translations file by file and **does not list `audio.c`**.
`NOTICE.md` attributes it to PPSSPP `Core/HLE/sceSasCore.cpp` and `Core/Audio/`, and the file body
cites neither upstream. That claim is now *uncorroborated by the immediate upstream's own
itemization*, which is evidence but not disproof. It stays `unresolved` pending a PPSSPP comparison.

### 5. The recompiler pipeline is substantially inherited and carries no attribution

This corrects an overclaim in the prior ledger, which called the pipeline "overwhelmingly
project-authored".

| file | shared lines | up_ret% | cur_of% |
| --- | ---: | ---: | ---: |
| `tools/codegen.py` | 617 | 83.9% | 37.5% |
| `tools/analyze.py` | 280 | 86.2% | 43.7% |
| `tools/prxload.py` | 81 | 73.6% | 23.4% |
| `tools/imports.py` | 47 | 71.2% | 19.8% |

None of these declares any derivation, and `NOTICE.md` has no per-file sal063 inventory. Counting
every file with ≥20 shared lines and no header, **3,111 shared lines are undeclared at file level**
— 1,481 of them outside `nid_names.h` (which `NOTICE.md` does cover, as data).

The project's defining technology is genuinely *extended* — codegen more than doubled, and function
discovery, chunking, continuation catalogs and the emitter are real additions — but it began as
sal063's and a large majority of sal063's pipeline code is still present.

## Open — requires a PPSSPP source comparison this audit did not perform

- Whether the retained portions of `audio.c` contain PPSSPP expression.
- Whether the four PPSSPP-attributed files that came through sal063 retain PPSSPP expression
  directly, or only sal063's re-expression of it.
- Nothing here reclassifies any file's *ultimate* origin. It establishes the *immediate* one.

## Reproducing

```bash
git clone --depth 50 https://github.com/sal063/PSP-recompilation-project "$CLONE_DIR"
```

Then run the comparison described under **Method** over the tracked intersection, with `$CLONE_DIR`
set to a scratch directory **outside the worktree**. The clone is not a dependency of any build or
gate, and must not be placed under `third_party/` in a tree that will be published.

# Pre-publication legal rewrite / isolation assessment

**Status: engineering risk assessment, not legal advice.** This maps what in the tree carries licensing or publication weight and identifies engineering choices that can reduce exposure. It deliberately does not declare the project legally cleared. Qualified review of the intended public tree remains required.

Companion to [NOTICE.md](../NOTICE.md), [PUBLICATION_READINESS.md](PUBLICATION_READINESS.md), [OSPS_BASELINE.md](OSPS_BASELINE.md), [KEY_HISTORY_SCRUB.md](KEY_HISTORY_SCRUB.md), and the canonical GitHub legal/provenance issues.

## What “maximize legality” means here

The useful objective is **not** to erase open-source obligations. It is to minimize infringement/anti-circumvention/trademark/privacy exposure while preserving truthful attribution and the ability to prove where every shipped byte came from.

High-value risk reduction therefore means:

- publish independently created interoperability tooling/runtime rather than game code/assets;
- satisfy every inherited open-source license/notice obligation instead of attempting cosmetic relicensing;
- exclude optional components whose exact rights are unresolved;
- isolate anti-circumvention-sensitive code and never ship keys/constants;
- sanitize history/privacy before public exposure;
- use modest compatibility branding and explicit non-affiliation;
- have qualified counsel review the **actual candidate tree/build**, not merely a summary.

## Derivation map

### Tier 1 — materially PPSSPP-derived source

These files contain translated/adapted PPSSPP algorithms, structures, tables or HLE logic and already carry PPSSPP derivation notices:

`src/rt/ge.c`, `src/rt/ge_shared.h`, `src/rt/hle.c`, `src/rt/mpeg.c`, `src/rt/savedata.c`, `src/rt/vfpu_interp.c`, `src/rt/pgf.c/.h`, `src/rt/recomp.c/.h`, `src/rt/evf.h` and related derived material identified by the #103 inventory.

For the PPSSPP-only portions, preserving the upstream GPL-2.0-or-later notice is the normal low-risk course. **The PGF subset is not settled by that statement**: PPSSPP's own PGF source records JPCSP copied-code lineage and says the file is effectively GPLv3; JPCSP in turn credits CC BY-SA 3.0 intraFont structure/constants. That chain is #98 and must not be papered over by a repository-wide GPLv2 statement.

### Tier 2 — behavioral/reference similarity requiring attribution review

`src/ref/interp.cpp` / `src/ref/cpu.h` mirror a number of PPSSPP integer/FPU semantics while their primary historical attribution is the sal063 toolkit. Whether this is merely implementation of public ISA behavior or incorporates protectable PPSSPP expression is a factual/source-comparison question. The conservative review is to preserve more attribution rather than less, but an SPDX/header edit is not a substitute for deciding what was actually derived.

The completed PGD/amctrl archaeology now places `tools/pgd_decrypt.py` and the
PSP-specific flow in `src/rt/pgd.c` beyond behavioral similarity: the flow is
**derived-translated** from the public tpu/JPCSP/PPSSPP/sign_np implementation family. No substantial
near-verbatim Nakagawa function body was found, and the exact source contribution to each block is
unrecoverable. See
[`provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md`](provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md).

### Tier 3 — PPSSPP used as oracle/reference

Tools/harnesses that compare externally observed behavior or consume trace formats are not automatically derivative merely because PPSSPP is the oracle. Keep provenance of copied formats/code separate from behavioral testing.

### Project-original / separately sourced areas

The SDL3/Vulkan backend is project-authored. The scheduler and most recompiler pipeline tooling are project-authored/adapted from the original sal063 project as documented. Within `src/rt/pgd.c` / `tools/pgd_decrypt.py`, the field-derived AES implementation and later defensive/runtime hardening are independently expressed; that narrower fact must not be broadened into independence of the PSP-specific PGD/amctrl flow.

## Do not perform a pseudo-clean-room rewrite to “escape” the GPL

People who already studied PPSSPP cannot retroactively make a defensible clean-room separation merely by rewriting syntax. For known PPSSPP-derived material, the safer route is generally to keep accurate attribution and comply with the applicable license.

Do **not**, however, say “GPL compliance is already settled” for the whole tree while #98/#99 remain open. The material question is the PGF/JPCSP/intraFont chain and redistributed font data, not whether GPL itself is undesirable.

## Highest-value legal isolation: PGD/amctrl

The current tree externalizes the game version key and PSP KIRK/amctrl constants, but `src/rt/pgd.c` remains compiled into the default native runtime. Omitting secret values is useful hygiene; it does not itself answer the legal question about distributing a circumvention/interoperability implementation.

Qualified review should compare at least:

1. initial public source with PGD/amctrl **excluded**;
2. source present but build/runtime opt-in and no keys/constants;
3. current compiled-by-default but keyless design;
4. any binary distribution containing the implementation.

If minimizing exposure is the priority, option 1 is now implemented as [`public-safe-v1`](PUBLIC_SOURCE_PROFILE.md). The core recompiler's host-neutral objects build with a fail-closed PGD-unavailable backend, while the private development configuration remains unchanged.

Relevant U.S. interoperability authorities include 17 U.S.C. §1201(f), *Sega v. Accolade*, *Sony v. Connectix*, and *Chamberlain v. Skylink*. They provide meaningful interoperability arguments under particular facts; they are not a blanket safe harbor for this implementation or for distribution in every jurisdiction. The current triennial video-game preservation exemption is narrow and should not be used as a generic emulator/recompiler publication theory.

## Fonts and PGF implementation

### PGF parser/renderer source (#98)

The chain presently requiring counsel/source comparison is:

Nakagawa PGF → PPSSPP PGF/sceFont → JPCSP PGF/SceFontInfo → intraFont-referenced structure/constants.

Archived intraFont is CC BY-SA 3.0. Creative Commons currently lists no non-CC compatible license for BY-SA 3.0; GPLv3 compatibility is a BY-SA 4.0 mechanism. This does **not** prove CC BY-SA applies downstream. It means the factual question “did protectable intraFont expression flow into the retained implementation?” must be answered before assuming GPLv3 alone resolves the chain.

### Redistributed PGF binaries (#99)

Byte identity to PPSSPP is proven. New upstream archaeology supplies Ume-font lineage evidence for `jpn0` and `ltn0`, but `kr0` and `ltn8` remain insufficiently tied to exact source-font/license releases. The public-safe profile therefore excludes all four binaries and the lineage-sensitive parser/rasterizer. Its unavailable backend fails visibly; the retired synthetic-font fallback is not treated as production behavior.

## VFPU data

The 15 `assets/vfpu/*.dat` files have exact PPSSPP byte provenance. PPSSPP issue #16946 / PR #16984 document the accuracy research and upstream contributor who introduced this table family. Preserve that PPSSPP provenance and applicable notices in any public package. These are not extracted game/firmware assets, but public availability alone is not the legal rationale; the upstream project contribution/license chain is.

## History/privacy — current tree cleanliness is not historical cleanliness

Do not repeat the old claim that no game/proprietary material exists anywhere in history.

Known facts include:

- PSP KIRK/amctrl constants were removed from the current tree but remain reachable in old history pending the mandatory scrub.
- #102 records an orphaned/force-pushed commit containing small verbatim retail-EBOOT disassembly snippets that remained resolvable by SHA on GitHub at the time of audit.
- commit metadata includes a personal author email and some AI-session URLs/identifiers that require a deliberate privacy decision.

The recommended public topology is a **fresh sanitized public repository** rather than flipping this historical private repository public. GitHub documents that private→public exposes Actions history/logs and disables push rulesets. Keep the development archive private; publish only an explicitly approved tree/history/refs and curated issues.

## Trademarks and naming

Current documentation uses *Hot Shots Tennis*, PSP, Sony and Clap Hanz names descriptively and includes a conspicuous no-affiliation/no-endorsement statement. That is directionally consistent with compatibility/nominative-use practice, but it is not immunity from a complaint.

For maximum risk reduction, have counsel review both the descriptive game-name use **and the project name “Nakagawa Recomp,”** because Nakagawa Tennis Club is itself an in-game identifier. A neutral generic project name is an available risk-reduction option if counsel recommends it; this assessment does not declare the present name infringing.

## Security/reproducibility affects publication risk too

A public tool that consumes malformed user-supplied binaries creates a different liability/reputation surface from a private research checkout. Before inviting arbitrary inputs:

- complete #15 to the release threat model;
- run synthetic sanitizers/fuzzers for ELF/PRX, SFO/savedata, PSMF/MPEG, PGD and archive extraction;
- pin optional extractor dependencies such as libxb and validate extraction containment in Nakagawa itself;
- generate an SBOM/dependency license inventory for the exact release;
- keep the dashboard loopback-only unless it is redesigned/authenticated for network exposure.

## Facts in good shape

- Private game/firmware/input paths are ignored from the current tree and publication audit exists.
- Keys/constants needed by PGD are not present in the current tree/build and the PGD path fails closed when local key material is absent.
- Third-party derivation and AI assistance are disclosed rather than concealed.
- SDL/Vulkan/dashboard upstream dependencies are separated from private game inputs.
- Current documentation explicitly states that publication is not legally cleared.

## Decision rule

Do not rewrite code merely to make the project *look* independent. Preserve accurate lineage. **Remove optional uncertain material from public scope, isolate sensitive interoperability components, sanitize history, and review the actual candidate release.** That creates a substantially more defensible posture than broad disclaimers or cosmetic SPDX edits.

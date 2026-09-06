# Competitive gap audit — PSP recompilation field (2026-09-05)

```text
STATUS = REFERENCE_SNAPSHOT
CURRENT_ARCHITECTURE_AUTHORITY = source + maintained architecture docs + live GitHub issues
COMPETITOR_FACTS = verify against source before implementation
```

Dated evidence record. This snapshot describes the field and Nakagawa's position as of the
review base below; it is **not** a timeless status document and does not supersede live source,
`docs/ARCHITECTURE.md`, `docs/PORTING.md`, the dated provenance/research records, or live GitHub
Issues. If this file disagrees with any of those, the live authority wins. Competitor facts were
taken from a source-level verification pass at pinned commits (Appendix A) and from public
release notes where noted; re-verify at the implementation commit before building on any idea.

Review base:

- Nakagawa worktree branch `research/competitive-audit-2026-09-05`, base `12bcc522...`
  (`origin/main` at `312d1d5d...` when fetched 2026-09-05).
- Competitor clones: `jessicanataliagta/PSPRecomp` `f6e7d41`; `sp00nznet/psprecomp` `caca759`;
  `wizardengineer/psprecomp` `bd3c33e`; `N64Recomp/RecompFrontend` `b1a1477` (shallow, default
  branch HEADs).
- Companion actionable register: `COMPETITIVE_ACTION_REGISTER_2026-09-05.md` in this directory.

No builds, tests, or hardware runs were performed for this audit. Nothing here authorizes a tag,
release, asset, or publication.

## 1. Position statement

Nakagawa is the deepest single-title PSP recomp bring-up in the field (private HST route plus a
source-owned public ladder) and simultaneously the least distribution-complete. Its durable
strengths are evidence discipline (hardware-measured semantics on PSP-3001/6.61-ARK), a
fail-closed AOT/interpreter execution contract proven by cosimulation with mutation kills, and
machine-enforced governance (provenance ledger, public-source profile, publication audits,
Betterleaks, path-gated CI). Its honest gaps: no end-user product shell, no in-game menu or
config persistence, no modding or texture-replacement surface, Windows-only link today, one
large `hle.c`, and remaining inventoried exact-address hooks in generic core.

## 2. Destination model: original guest execution first (corrected framing)

This section corrects an earlier draft that framed the field's real distinction as "generic
ownership versus title ownership." That is incomplete. The architectural destination is biased
toward genuine original-guest execution:

```text
original PSP guest machine code / genuine PRX execution
  > measured low-level PSP platform behavior
  > generic HLE only where a host boundary genuinely requires it
  > title-specific HLE
  > patch / workaround
```

The current runtime HLEs much of the PSP kernel and user middleware; that is **today's
implementation, not the architectural ceiling**. Consequences for planning:

1. **Kernel service surfaces** (`sceKernel*`, scheduler, interrupts, waits) are host boundaries
   in every known PSP recomp project; HLE there is expected. What must be measured and modelled
   (not approximated) is the *low-level behavior beneath* those boundaries: clocks, VCOUNT/
   VBLANK delivery, interrupt gating, wait/wake semantics, callbacks. This is Nakagawa's existing
   moat and the layer directly under "measured low-level PSP platform behavior."
2. **Genuine guest modules should execute when practical.** Where a PSP user module (for
   example libfont, PSMF/`scePsmf*` middleware, or a future dynamic PRX) is supplied as guest
   code, the destination model prefers running that guest code and HLE-ing only the true kernel/
   service calls it makes — rather than permanently replacing the module with a host
   implementation merely because replacement is easier. Every existing host replacement
   (`mpeg.c`-class media handling, font paths, module-load interception) should be re-examined
   against this hierarchy and classified: genuine host boundary (kernel service, codec
   hardware, presentation) versus convenience replacement that a faithful port would outgrow.
3. **Title-specific HLE and patches sit at the bottom**, below generic HLE. They remain
   legitimate but are last resorts with evidence, inventory, and removal criteria — not the
   normal answer to difficult guest code (see section 6 on fast paths).
4. **Dynamic module loading is part of the destination.** Boot-time module sets are a special
   case of "genuine PRX execution." A runtime that can load, relocate, and execute modules after
   boot (including title-loaded PRXs) is closer to the destination than one that only supports a
   manifest-declared boot set.

Every recommendation in this audit is phrased against this hierarchy. In particular, the
"profile model" comparison (section 4a) and the fast-path policy (section 6) are subordinate to
it: profiles primarily *route guest modules and bindings*; host fast paths are a last resort.

## 3. Competitor profiles (source-verified)

| Project | License (verified HEAD) | Core architecture (verified) | Flagship state |
| --- | --- | --- | --- |
| sal063/PSP-recompilation-project (Nakagawa's ancestor) | GPL-2.0-or-later (PPSSPP-derived) | Python offline tools → generated C; original SDL3+Vulkan renderer; PPSSPP-derived HLE/GE/MPEG | Toolkit; per-title output. Nakagawa lineage. |
| jessicanataliagta/PSPRecomp | MIT (`LICENSE`) | C++20 framework + per-title profiles; generated AOT corpus checked into `profiles/vcs/generated/` (~237 files + registry + `auto_codegen_report.json`); runtime registration API (`include/psprecomp/runtime.hpp`) with `register_function`, `register_hle`, `register_native_fast_path`/`invoke_native_fast_path` (`src/runtime.cpp`); address-specific lowering lives in profile tools (`profiles/vcs/tools/vcs_codegen_main.cpp`), not root codegen | VCS (GTA Vice City Stories) profile; Windows/DX12 host; private/active |
| sp00nznet/psprecomp | MIT (`LICENSE`) | C toolkit `allegrexrecomp` (container peeling, KIRK decrypt with **external, user-supplied key file** — `tools/allegrexrecomp/keys.c`/`keys.h`, "kirk1" 16-byte key, `keys/` gitignored); runtime with cache-mirror collapse (`src/mem.c:85`); dispatch instruments (`src/dispatch.c`: entry trace, call budget, loop back-edge, label reachability); HLE per firmware module under `src/hle/`; run-to-completion scheduler (no preemption); 10+ ctest suites | Retail title compiles, links, reaches allocator; threading/rendering incomplete |
| wizardengineer/psprecomp | GPL-2.0-or-later (`LICENSE`) | Rust analyze (Ghidra headless census + own ELF parsing) → C++17; generic runtime + per-game module (`games/patapon`, `games/dothack`, `games/TEMPLATE`); **mechanical purity gate** `runtime/tools/purity_gate.sh` (game-range literal allowlist + `nm` object-symbol audit of core objects); census guards `--expect-functions`/`--expect-mid-entries` (`crates/psp-cli/src/recompile.rs`); generated per-game syscall/NID tables; PPSSPP as scriptable behavioral oracle; TCP debug socket on 9999 (`runtime/src/main.cpp`) | Patapon boots + renders title menu (macOS); `.hack//Link` config-only boot to loading screen; no audio; partial GE |
| N64Recomp/RecompFrontend | No `LICENSE` at verified HEAD (only bundled `lib/GamepadMotionHelpers/LICENSE`); **verify before reuse** | `recompui` built on RmlUi (`recompui/include/recompui/recompui.h` includes `RmlUi/Core.h`; font registration, mod list management `update_mod_list(bool scan_mods)`); `recompinput` (SDL2 input, per-player binding/profiles); renders UI through RT64 plume command lists (README) | Library powering Zelda64Recomp-class titles |

Lineage note: no declared GitHub fork relationship or documented common lineage with
Nakagawa/sal063 was established in this review; only sp00nznet's docs reference sal063 as prior
art. This does not rule out inspiration, manual code transfer, or undocumented derivation, so
inherited-feature discounting is not applied either way. Declared PPSSPP lineage does exist in
the GPL projects (wizardengineer declares PPSSPP-derived semantics; sal063-derived code is
GPL).

## 4. The two flagged questions

### 4a. PSPRecomp's profile model vs. Nakagawa's doctrine

PSPRecomp makes the profile the **unit of organization**: everything address-specific — HLE
host, renderer/audio/input integration, generated corpus, native leaves — lives under
`profiles/<id>/`, root codegen stays generic, and a framework build with no profile must work.
Nakagawa's typed title manifests (`assets/titles/*.json`, `runtime_bindings`) plus the
machine-inventoried compat surface (`tools/compat_overrides.py`, issue #98) are the same idea
with an extra layer of evidence discipline.

Verdict, now framed against the destination model:

- **ADAPT (high priority):** profile ergonomics that make *guest-module routing and title
  bindings* first-class and legible — thin profiles, a "framework builds with no profile" CI
  invariant, reproducible profile corpora kept local (never public retail-derived bytes), and
  census-drift guards.
- **REJECT as policy:** "whatever a profile needs, forever," and in particular the idea that a
  title-specific *native replacement of guest code* is a normal first answer. Under the
  destination hierarchy, profiles replace guest code only after original-guest execution or
  generic HLE has been shown impractical, with evidence attached.
- **Long-term ceiling:** PSPRecomp's model optimizes ecosystem velocity but has no force toward
  generalization (per-title host work duplicates; shared value converges slowly). Nakagawa's
  model optimizes generic correctness convergence but only pays off if generalization keeps pace
  with title adoption. The winner over the next year is whoever lands a **second title with the
  core demonstrably reusable** — which is why second-title genericity proof is investment #1
  (section 8), not any framework restructuring.

### 4b. RmlUi-class frontends vs. Nakagawa's hand-built SDL3 layer

The premise holds: Nakagawa's `gui.c` + `gpu_sdl3vk/` is a *presenter*, not a *frontend*; the
Next.js studio dashboard is an out-of-process developer console and does not ship in `hst.exe`.
There is no in-process overlay, settings UI, config store, or controller-reachable menu.

This is intended to become a **polished consumer application**, so "minimal own stack" must not
win by default. Candidate UI engines should be spiked and compared on the full axis list —
visual quality, animation, typography, DPI, controller navigation, keyboard/mouse,
accessibility, IME/text entry, localization, theming, mod-supplied UI, Vulkan integration,
Windows/Linux/macOS, x86-64/ARM64, bundle size, maintenance burden, licensing — before any
custom UI hardens:

- **RmlUi** (proven in Zelda64Recomp-class in-game menus; HTML/CSS; MIT license family — verify
  pinned version)
- **Slint** (native, declarative, strong embedded/desktop story; licensing needs review at
  pinned version)
- **Qt/QML** (mature, best typography/localization/accessibility/IME story; heavyweight
  dependency)
- **Dear ImGui** (excellent for developer/debug UI; not presumed suitable for the polished
  player UI)
- **custom retained-mode SDL3 UI** (only if spikes show the others cannot meet controller +
  theme + mod-UI needs at acceptable size/cost)
- The **launcher** is a separate surface; a webview/Tauri-class shell (OpenGOAL launcher style)
  belongs there, not in the game process.

Recommended sequencing: (1) decouple the presenter from frontend duties behind a host-frontend
seam with persisted config and input remap; (2) run UI-engine spikes on the axis list; (3) adopt
one engine for in-game menus; (4) define the mod-UI API shape before community mods exist.

## 5. Retail container and KIRK: INVESTIGATE / LIKELY ADAPT (P0 enabling architecture)

Earlier draft conclusion ("REJECT shipping decryption") is withdrawn. The common-user target is
`Nakagawa + user's lawful PSP ISO -> Play`, and a clean, legally distributable,
provenance-safe local processing path for the retail container may be essential to that. The
questions must be separated, not conflated:

1. ISO/container parsing (ISO9660 read of the UMD; Nakagawa already has private `iso.c`; PBP
   parsing is absent — a source `EBOOT.PBP` is currently unused input).
2. `~PSP` / `~SCE` module envelope parsing (already separable; format publicly documented).
3. PRX/EBOOT transformations (rebase, relocations, BSS recovery — Nakagawa already does this in
   `tools/prxload.py` on decrypted input; the step does not depend on the crypto).
4. KIRK algorithms (AES-CBC and the command/CMAC variants; the command interface is publicly
   documented and independently implementable from published documentation).
5. Cryptographic key material — separately from 1–4:
   - which keys are title-specific, firmware-specific, or device-specific;
   - which keys/algorithms are already publicly documented constants vs. extracted secrets;
   - where a user lawfully obtains required key material.
6. Copyright/license/provenance status of any implementation and of key material handling.
7. Whether user extraction of anything is actually necessary, or the tool can accept the retail
   container directly.

Verified field facts that bound the design space (not a resolution):

- sp00nznet/psprecomp implements the full KIRK decrypt path in-repo but **never bundles key
  material**: it loads an external user-supplied file (`--keys` / `$PSPRECOMP_KEYS` /
  `keys/psp_keys.txt`) and requests one named 16-byte key, `kirk1`, for retail-module
  decryption (`tools/allegrexrecomp/keys.c`/`keys.h`, callers in `main.c`). Its header comment
  calls PSP decryption constants "published facts" while still refusing to distribute them.
- Public decryption tooling exists and is actively maintained (e.g., `John-K/pspdecrypt`
  `PrxDecrypter.cpp`, PRXDecrypter lineage, a 2026 Rust port `0xLozi/r-psp-decrypter`).
- The per-title/firmware/device specificity of required keys, and the lawful acquisition path,
  remain open sub-questions this audit does **not** resolve. Do not guess either direction.

Decision status for the register: `INVESTIGATE / LIKELY ADAPT` — P0 enabling architecture, with
the sub-question list above as the investigation brief. Nakagawa's public-source and provenance
rules already distinguish "implementation in-tree" from "key bytes in-tree"; the design should
preserve that boundary.

## 6. Native fast-path policy (corrected — conditional last resort, not P0 pillar)

PSPRecomp's mechanism is real and instructive: `register_native_fast_path(address, fn)` /
`invoke_native_fast_path(address, ctx)` keyed on canonical guest address
(`include/psprecomp/runtime.hpp:113-114`, `src/runtime.cpp:1105-1148`); generated code in the
VCS profile calls it at hard-coded sites emitted by the profile's own codegen tool
(`profiles/vcs/tools/vcs_codegen_main.cpp`); unregistered targets fall back to the AOT body; and
the profile's native leaves carry **differential self-validation** against the generated AOT
reference — whole-context `memcmp`, `PSPRECOMP_NO_FAST_<addr>` kill switch, and
`PSPRECOMP_VALIDATE_FAST_<addr>` for N consecutive real calls
(`profiles/vcs/host/vcs_native_fast_paths.cpp`). Notably, its two examples are *bit-exact host
lowerings of guest leaves* that were already AOT-validated — closer to Nakagawa's inventoried
narrow-translation debt than to arbitrary replacement.

If Nakagawa ever adds such a mechanism, the policy is:

- optional optimization only (disable must still work — PSPRecomp's fallback proves it is
  achievable);
- semantics must already be established (generated AOT or original guest is the reference);
- original guest execution remains the correctness reference;
- generic/runtime implementation preferred over title-specific replacement;
- title-specific native replacements are explicitly inventoried;
- evidence/provenance attached (differential validation of the PSPRecomp kind or better);
- performance justification measured;
- removal path available;
- never the platform's normal answer to difficult guest code.

Priority: **P2, conditional** — behind second-title genericity, guest-module execution, and
hardware conformance. It is deliberately absent from the top tier of section 8.

## 7. Interpreter role (corrected)

The fail-closed interpreter floor (`src/rt/guest_interp.c`, enumerated subset, CALL/TAIL
boundary contract; issues #116/#118/#127/#128) is valuable **because it is a correctness floor**,
not because Nakagawa should gradually become PPSSPP with AOT in front of it. Expanding it toward
broad PSP coverage is not a top-tier investment and must not become the main compatibility
strategy. Extend it only when it closes a proven correctness hole, and weigh every proposed
extension against higher-leverage alternatives first:

- improving AOT coverage (fewer legitimate misses);
- dynamic module / original-PRX execution and loading;
- callable/resume-entry correctness;
- module relocations/import/export handling;
- lower-level PSP semantics (the measured layer).

The floor's current value — executing legitimate analyzer-owned AOT misses rather than
fabricating success — should be preserved and defended as-is.

## 8. Revised top-15 investments (ranked by platform leverage)

Items 1–10 are the first tier (months 0–6); items 11–15 are the second tier (months 6–12).

1. **Second-title genericity proof through the generic core.** Bring a second real title to a
   defined milestone with no un-inventoried core edits; the mechanical purity gate (register
   item B1) is the enforcing evidence. Converts doctrine into platform fact.
2. **Retail ISO → genuine guest-code preparation architecture** (section 5): container →
   module → relocations → guest image inside the tool, with the implementation/key-material
   boundary preserved. INVESTIGATE then likely ADAPT; the enabling architecture for the
   consumer target.
3. **Dynamic module / original-PRX execution.** Runtime loading, relocation, import/export, and
   execution of genuine guest modules (not just the manifest boot set); drives the
   original-guest-execution destination and unblocks middleware like PSMF/libfont under the
   destination model.
4. **Hardware-conformance expansion.** Productize `tools/psp_oracle` into a reproducible,
   versioned conformance suite growing the measured matrix (audio, UMD/ISO latency, GE
   interrupts, waits/intr rows). The moat no competitor has.
5. **Cross-platform / host-abstraction architecture.** Extract Win32 from `hle.c` and friends
   behind the host layer; make the x86-64-free architectural contract explicit now so ARM64 and
   macOS do not become afterthoughts; Linux end-to-end first, ARM64/macOS gates as portability
   evidence lands.
6. **Canonical build architecture (single source of truth).** Investigate and choose one
   canonical cross-platform build (CMake + Ninja recommended over Meson; Make retained only
   during a transition, then retired — never two permanent build systems). IDE integration,
   generated-source handling, packaging, and contributor experience are the criteria.
7. **Production-grade frontend architecture** (section 4b): presenter decoupling, config
   persistence, then UI-engine spike and adoption. The product shell for a consumer app.
8. **New-title automation.** `new-title` scaffold: manifest + build plan + census/import/NID
   diff report + expect-baselines + documented walkthrough; manager generalizes off the
   HST-only acceptance.
9. **Mod architecture designed before ABI ossification.** Hooks, config schema, mod-supplied
   UI API, boot-time load — gated by title isolation; designed now, shipped when the community
   surface exists.
10. **Security/fuzzing of every user-controlled input.** Sanitizer CI jobs + seeded corpus over
    ELF/PRX/PBP/ISO/route/manifest parsers; completes the declared parser/span hardening
    campaign.
11. **(Tier 2) Local deterministic AOT corpus caching** (digest-pinned, profile-scoped, never public
    retail-derived bytes) with staleness fail-closed; fixes build economics for title #2+.
12. **Mechanical purity enforcement, strong form** (register item B1): game-range literal
    allowlist + object-symbol boundary audit + runtime null-profile census, replacing reliance
    on source greps; folds into #1 but is worth naming as its own workstream.
13. **Texture-replacement architecture** (hash-keyed override keyed on VRAM/CLUT/swizzle decode
    in the GE sampler) designed before the sampler keys ossify.
14. **Crash/support bundles + one-command repro**: structured crash report → auto-derived
    route replay, save-baseline integrity, symbolication.
15. **Controller-first UX and config persistence** (remap, deadzone, per-device profiles)
    riding on the frontend architecture (#7); plus process discipline: dated research snapshots
    and live GitHub Issues/Milestones as the only status authority — no new manually maintained
    roadmap documents.

Standing policy notes that outrank any single investment: the interpreter is a correctness
floor (section 7); native fast paths are conditional last resorts (section 6); documentation
records architecture and dated findings, never duplicates mutable tracker state.

## Appendix A — verification record

Source-level verification pass performed 2026-09-05 over shallow clones (default-branch HEADs).
Everything marked "verified" below was read at the pinned commit; everything else is labeled.

| Claim | Repo | Commit | Evidence | Verified |
| --- | --- | --- | --- | --- |
| Framework MIT | jessicanataliagta/PSPRecomp | f6e7d41 | `LICENSE` | yes |
| Fast-path registration API | same | same | `include/psprecomp/runtime.hpp:83-84,113-114`; `src/runtime.cpp:1105-1148` (canonical-address map; unregistered falls back to AOT body) | yes |
| Profile codegen owns address-specific lowering | same | same | `profiles/vcs/tools/vcs_codegen_main.cpp` (hard-coded invoke sites); `docs/PROFILE_GUIDE.md` | yes |
| Fast paths differentially self-validate | same | same | `profiles/vcs/host/vcs_native_fast_paths.cpp` (`collision_fast_path`, `vfpu_fast_path`; `PSPRECOMP_NO_FAST_*`/`VALIDATE_FAST_*`; whole-context `memcmp` vs `invoke_isolated_aot`) | yes |
| Checked-in generated AOT corpus | same | same | `profiles/vcs/generated/` (~237 units + `generated_registry.cpp` + `auto_codegen_report.json`; no per-file license header) | yes |
| Toolkit MIT; keys external | sp00nznet/psprecomp | caca759 | `LICENSE`; `tools/allegrexrecomp/keys.c`/`keys.h` (external file, `kirk1` 16-byte, `keys/` gitignored) | yes |
| Cache-mirror collapse | same | same | `src/mem.c:85` | yes |
| Dispatch instruments | same | same | `src/dispatch.c` (entry trace, call budget, loop back-edge, label reachability) | yes |
| Run-to-completion scheduler | same | same | `src/hle/threadman.c` header (no preemption; waits return timeout) | yes |
| Per-game layer + purity gate | wizardengineer/psprecomp | bd3c33e | `games/{patapon,dothack,TEMPLATE}`; `runtime/tools/purity_gate.sh` (literal allowlist `ALLOWED_RE` + `nm` object-symbol audit) | yes |
| Census drift guards | same | same | `crates/psp-cli/src/recompile.rs:87-89,393`; `crates/psp-cli/src/report.rs:67` | yes |
| TCP debug socket | same | same | `runtime/src/main.cpp` (loopback 9999) | yes |
| License | same | same | `LICENSE` (GPL-2.0-or-later) | yes |
| RmlUi-based recompui | N64Recomp/RecompFrontend | b1a1477 | `recompui/include/recompui/recompui.h` (RmlUi includes, font registration, `update_mod_list(bool)`) | yes |
| License | same | same | no `LICENSE` at HEAD (only bundled `lib/GamepadMotionHelpers/LICENSE`) | no — verify before reuse |
| N64Recomp patch/group modes, Zelda64Recomp mods/texture packs, OpenGOAL launcher | N64Recomp / Zelda64Recomp / open-goal | n/a | public READMEs and release notes only in this pass | no — source pass required before implementation basis |

## Appendix B — Nakagawa-side honesty notes

- At review base, `src/rt/recomp.c` still carries inventoried exact-address dispatch hooks
  (`g_exact_hooks[]`, `INIT_LANG` at `0x00304290`, `INIT_WALKER_GUARD` r16 save/restore),
  classified COMPATIBILITY_PROFILE_REQUIRED by the dated census
  (`docs/provenance/GENERICITY_CENSUS_20260826.md`, revalidated 2026-08-27; recheck against live
  source — later waves migrated several other groups to typed `runtime_bindings` per
  `docs/PORTING.md`).
- `hle.c` is ~10.9k lines; the dated census and issue #98 remain the authoritative coupling
  inventories.
- The public tree cannot run the private HST route; the source-owned ladder (`production-smoke*`,
  `cosim-selftest`, `platform-ladder`, native selftests) is the honest inner loop.

## Appendix C — process commitments (leadership corrections incorporated)

- No new "timeless" status documents; dated research snapshots only, in
  `docs/research/competitive/` and the existing dated-evidence convention.
- Live work authority is GitHub Issues/Milestones/Projects plus machine-checked invariants, not
  a manually maintained roadmap table inside documentation.
- Every competitor-derived idea in the register carries exact source evidence and an
  ADOPT/ADAPT/REJECT/INVESTIGATE verdict so nothing is adopted from README claims alone.

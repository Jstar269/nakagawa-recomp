# PGD/amctrl qualified-review packet

**Status: engineering facts for qualified legal review, not legal advice.** This packet supports qualified legal review for PGD/amctrl distribution posture. It was reconciled on 2026-08-09 against `main` at `72fe8739b77f6dc2d255544651849476b463e55e`; counsel should review the cited source and intended release tree, not rely on this summary alone.

## Decision requested

Please advise separately on these configurations rather than treating “the repository” as one indivisible product:

1. **Initial generic public source with PGD/amctrl excluded** from the public tree/build.
2. Public source containing the implementation as an explicit, default-disabled component, with no keys/constants/game content.
3. The current development architecture, where `src/rt/pgd.c` is compiled into the normal runtime but cannot operate without user-local PSP constants and a title version key.
4. Distribution of a native binary containing the implementation but none of those omitted values or game content.
5. Any later configuration that distributes or automates acquisition of key material (not currently planned).

If minimizing legal exposure is the primary objective, configuration 1 is the engineering default until written review says otherwise. The project does not treat source clearance as binary-release clearance.

## What the code implements

| Surface | Current behavior |
| --- | --- |
| `tools/pgd_decrypt.py` | Standalone, dependency-free Python reference/CLI. Parses PGD headers, verifies MACs and decrypts payload blocks to a local output selected by the user. |
| `src/rt/pgd.c` / `pgd.h` | C port of the project's Python reference. Computes AES-128 primitives, fixed-key KIRK command 4/7 behavior, BBMac and BBCipher flow, verifies headers and decrypts requested blocks. |
| `src/rt/hle.c` | Handles PSP I/O control `0x04100001`, obtains the 16-byte title version key from guest memory, reads the encrypted header, attaches a successful `SrPgd` context to the open file and exposes decrypted reads. Failure follows the title's invalid-header/fallback path. |
| `Makefile` | Includes `src/rt/pgd.c` in both normal and portable-core source sets. The current development executable therefore embeds the implementation even when keys/constants are absent. |

The supported runtime path is the fixed-key `drm_type == 1` form used by the target install cache. The Python reference explicitly rejects forms requiring a per-device fuse key. The C entry point fails closed on missing constants/version key, unsupported or corrupt headers, MAC failure, invalid sizes and read/allocation failures.

## Inputs and data boundaries

| Input | How it enters | Publication posture |
| --- | --- | --- |
| PSP KIRK/amctrl constants | Local `$SR_PGD_KEYS` / ignored `keys/pgd_keys.txt` | Absent from current tree/build. Known old-history copies are part of #102 + the coordinated scrub. Do not publish values or acquisition instructions. |
| Title version key | Guest memory; CLI `--vkey`; private tests via `HST_PGD_VKEY_HEX` | Not tracked. Tests skip when absent and must never log it. |
| Encrypted `GAMEDATA.BDL` | User's ignored savedata/install data | Not tracked/released. |
| Decrypted output | Local CLI output or runtime block buffer | Not tracked; forbidden from public issues/releases. |
| Synthetic constants/fixtures | Generated tests | Invented values that decrypt no retail content; suitable for public regression tests. |

`docs/PGD_KEYS.md` documents only the local schema. It intentionally provides no real values and no extraction/source instructions.

## Source and implementation provenance

The completed engineering archaeology no longer classifies the complete implementation as
independent. The maintained classification is:

- **Python first, then C port — high confidence.** Recovered July 18 private-archive timestamps,
  byte-identical root-import blobs, tests, and contemporary history establish the order.
- **PSP-specific BBMac/BBCipher/PGD flow — derived-translated.** Substantial staged logic was
  expressed after consultation of the public amctrl/PGD implementation family.
- **AES primitives — independently expressed standard cryptography.** The S-box and round constants
  are computed from the field definition and checked against the official AES known-answer vector.
- **Later validation, overflow, allocation, streaming, cleanup, key externalization, and public-safe
  seam — independently expressed project engineering.**
- **No substantial near-verbatim Nakagawa function body was found.** That source-shape result does not
  turn a translated high-level flow into an independent implementation.

The complete timeline, Proxima/Draan split, expression matrix, archive addendum, and remaining limits
are in
[`provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md`](provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md).

Technical references presently identified:

- NIST, [FIPS 197: Advanced Encryption Standard](https://csrc.nist.gov/pubs/fips/197/final).
- Draan's original KIRK engine as preserved by hexxellor, beginning at
  [`d1fa7cd591cdfc46cf1b1c61044566d0076f7b68`](https://github.com/hexxellor/kirk-engine/commit/d1fa7cd591cdfc46cf1b1c61044566d0076f7b68).
- tpu's January 2011 Fake_NP source package, the earliest recovered C amctrl implementation; the
  archive and its no-license finding are identified by hash in the source-archaeology report.
- JPCSP's first recovered combined amctrl/PGD implementation at
  [`5d41d35d900ae2d44355f17295d653c535d0ff16`](https://github.com/jpcsp/jpcsp/commit/5d41d35d900ae2d44355f17295d653c535d0ff16).
- PPSSPP `ext/libkirk/amctrl.c` at pinned upstream revision `f0baf3ade7bcb6c86f0835962b36eb4e51559d8f`.
- tpunix `kirk_engine` at `ee09e86b743d1c147579ff21f46dd0874303daf3`; the inspected files have no established reusable-license grant. They are part of the public implementation lineage, while no substantial near-verbatim Nakagawa body was traced to that snapshot.
- `sign_np` at `ac77d56e13b5c6c60564100699560d12ff3f425f`; its repository contains GPLv3 license text and its amctrl/PGD code is part of the compared public lineage, not a standalone license conclusion for Nakagawa.
- ProximaV `kirk-engine-full` from
  [`5561812233f81e7c4b4fdc76c89a2b6c783d8c7a`](https://github.com/ProximaV/kirk-engine-full/commit/5561812233f81e7c4b4fdc76c89a2b6c783d8c7a)
  through current `master` `3eb9bf14108215612f12e47907f9bb6a0c16394a`; it is KIRK-core evidence only,
  not a high-level amctrl/PGD ancestor.
- PSP Developer Wiki PGD format documentation as secondary technical context.

A qualified licensing review should use the derived-translated classification and the pinned source
record rather than the earlier implementation-independence claim.

## Earlier follow-up — 2026-08-04: source-to-source comparison (superseded)

This comparison is retained because it supplied useful source-shape evidence, but its conclusion that
the exact origin was unknown is superseded by the full-history archaeology and private-archive
reconciliation above. It is not a license determination, anti-circumvention opinion, or permission to
publish.

| Surface | Nakagawa implementation | Public comparison | Evidence-grade conclusion |
| --- | --- | --- | --- |
| AES primitives | `src/rt/pgd.c` computes the GF(2^8) multiplication, inverse, S-box and round constants and expands AES-128 keys locally. | PPSSPP, `kirk_engine` and `sign_np` route through their libkirk/AES helpers rather than the same table-free decomposition. | **Source-shape difference;** the mathematical operation is standard AES and does not establish independent authorship by itself. The local AES known-answer and Python/C tests are production-helper/model evidence for behavior only. |
| BBMac | `cmac_shift`, `bbmac_cbc`, `bbmac`, and `bbmac_verify` implement 16-byte CBC-MAC streaming, padding, optional version-key processing and the key-63 verification stage. | PPSSPP [`amctrl.c` at `f0baf3a`](https://github.com/hrydgard/ppsspp/blob/f0baf3ade7bcb6c86f0835962b36eb4e51559d8f/ext/libkirk/amctrl.c) exposes `sceDrmBBMacInit/Update/Final/Final2` and `bbmac_getkey`; the same stages are present in [`kirk_engine`](https://github.com/tpunix/kirk_engine/blob/ee09e86b743d1c147579ff21f46dd0874303daf3/kirk/amctrl.c) and [`sign_np`](https://github.com/swarzesherz/sign_np/blob/ac77d56e13b5c6c60564100699560d12ff3f425f/libkirk/amctrl.c). | **Derived-translated PSP flow.** The source expression differs, but the staged organization was implemented after consulting this public family. |
| BBCipher | `bbcipher_tmp2` derives the temporary key through the header key, the first named constant, KIRK7 key 39 and the second named constant; `bbcipher_apply` derives a counter/seeded key-63 block and XORs the stream incrementally. | PPSSPP `sub_1F8`/`sub_428`, `sceDrmBBCipher*`, and `sign_np` `cipher_buf`/`encrypt_buf`/`decrypt_buf` expose the same staged flow. | **Derived-translated PSP flow** for the examined mode; later constant-stack/streaming hardening is independently expressed. |
| PSP constants | Nakagawa reads the three named 16-byte constants from the user-local ignored key schema and does not embed their values. | The pinned PPSSPP and `kirk_engine` sources declare the same three named arrays; `sign_np` carries the corresponding amctrl data in its libkirk implementation. | **Exact public-reference constant-name/usage correspondence;** no conclusion about source copying or the provenance of the local values. Do not reproduce values in public documentation. |
| PGD layout and validation | `sr_pgd_open` checks the magic and supported fields, verifies MACs at the header offsets, decrypts the parameter block, validates checked sizes before allocation, and reads through a bounded block cache. | [`sign_np/pgd.c`](https://github.com/swarzesherz/sign_np/blob/ac77d56e13b5c6c60564100699560d12ff3f425f/pgd.c) shows the corresponding PGD header fields, MAC order and encrypted data/header flow, while also supporting broader tool modes and less defensive pointer handling. | **Derived-translated PGD flow; independently expressed defensive/runtime hardening.** |
| Source expression | Nakagawa keeps a Python implementation and a direct C port; function names, comments and temporary-buffer layout differ from the comparison files. | A normalized comparison of substantive comment lines between Nakagawa `pgd.c` and each of the three pinned public `amctrl.c` files found no exact intersections. | **No substantial near-verbatim body found.** Distinct syntax/helper layout does not establish independent implementation ancestry. |

The superseding investigation classifies the fixed-key BBMac/BBCipher/PGD flow as
**derived-translated**, while leaving the precise source-by-source contribution unknowable. `sign_np`
contains GPLv3 license text at the pinned fork commit; that is a fact about that repository, not a
license conclusion for Nakagawa. The inspected tpunix `kirk_engine` snapshot has no established
reusable-license grant in its tree. Retain this distinction in any future notice or publication
packet.

The comparison does not authorize distribution of keys, constants, encrypted/decrypted game data or acquisition instructions, and it does not resolve the legal questions listed below. Any release decision still requires qualified human legal review of the actual candidate tree and build configuration.

## Publication-lane update — 2026-08-06

Current-head facts for the reviewer, verified at export commit `dd0bcaea` during the
2026-08-06 publication-lane pass:

- The `public-safe-v1` candidate **excludes the entire PGD/amctrl surface**: `src/rt/pgd.c`,
  `src/rt/pgd.h`, `tools/pgd_decrypt.py`, `tools/pgd_e2e_harness.c`, `tools/pgd_test_keys.py`, and the
  four `tools/test_pgd_*.py` tests are all filtered from the export tree
  (`assets/public_source_profile.json`); the Makefile defaults to `PUBLIC_SAFE=1` there and the
  `pgd_unavailable.c` stub fails closed. The exported tree builds (`public-safe-verify` exit 0) and
  its candidate-tree audit reports 0 findings.
- No keys, title version key, encrypted retail file, or decrypted output are in the tree or the
  export; `docs/PGD_KEYS.md` continues to document only the local schema. `tools/verify_key_scrub.py`
  still reports the known historical reachability (exit 3), which is why the public repository is
  constructed as a fresh sanitized single-commit export rather than by exposing this history.
- The configuration-1 posture (initial generic public source with PGD/amctrl excluded) is the
  engineering default this pass validated. Configurations 2–5 remain future decisions for qualified
  review.

The packet's questions for counsel (1–8) remain open under #104. Nothing in this update changes the
exclusion posture or any anti-circumvention analysis. It predates the 2026-08-09 source archaeology
(reconciliation PR #341 and `provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md`) that
reclassified the PSP-specific BBMac/BBCipher/PGD flow from "independently claimed, to verify" to
"derived-translated"; that reclassification does not alter this section's engineering facts or the
exclusion default.

### Supporting research-corpus context (PSPRecompWiki, section 06, doc 90)

Architectural context from the project's research corpus (wiki doc 90, "PSP Security and
Protected-Content Architecture") frames this packet's configuration question without adding legal
conclusions:

- The wiki's protection-layer taxonomy separates boot trust, firmware/module executable protection,
  game/package/content protection, **savedata/content-authentication services** (where the fixed-key
  BBMac/BBCipher flow sits), device identity, and DRM/account identity. The PGD path under review
  belongs to the savedata/content-authentication layer, not to boot or firmware emulation; the
  implementation is a fail-closed runtime that consumes an already-encrypted user-supplied header and
  the user's locally supplied constants.
- The wiki's publication/legal-engineering boundary lists what public-safe documentation may include
  (architecture diagrams, public API/format descriptions, interoperability behavior, error/status
  semantics, source-owned synthetic tests, provenance and source comparisons) and what stays out of the
  public corpus (device-specific secrets, secret-key collections, access-control-bypass instructions,
  proprietary retail modules, private savedata). This matches the packet's engineering posture: the
  algorithm-level comparison and the excluded-component profile are publishable; the constants,
  version key, and retail inputs are not.
- The wiki states as its core rule that understanding a protection layer does not mean the project
  needs to reproduce, distribute, or bypass it. That is the architectural rationale for the
  `public-safe-v1` exclusion, independent of the legal questions in this packet.
- The corpus's publication classification vocabulary (wiki doc 78) includes `private-key-secret`,
  `qualified-review-required`, and `do-not-publish`; the PGD constants and title version key are
  `private-key-secret`/`do-not-publish` under that vocabulary, while the excluded implementation is
  `qualified-review-required` until counsel rules on configurations 1–5.

## U.S. interoperability/circumvention authorities for counsel

These are authorities to evaluate, **not a project conclusion or safe-harbor claim**:

- **17 U.S.C. §1201(f)** expressly addresses certain reverse engineering by a lawful user to identify/analyze elements necessary for interoperability of an independently created computer program, and certain development/use/sharing of necessary means, subject to statutory conditions and other law.
- **Sega Enterprises Ltd. v. Accolade, Inc., 977 F.2d 1510 (9th Cir. 1992)** — software disassembly/intermediate copying for compatibility was fair use on the facts presented.
- **Sony Computer Entertainment, Inc. v. Connectix Corp., 203 F.3d 596 (9th Cir. 2000)** — intermediate PlayStation BIOS copying during reverse engineering was fair use where necessary to create a noninfringing emulator; the final product did not contain Sony copyrighted BIOS material.
- **Chamberlain Group, Inc. v. Skylink Technologies, Inc., 381 F.3d 1178 (Fed. Cir. 2004)** — rejected the asserted DMCA access-control claim on its facts and discusses authorization/copyright nexus/interoperability issues.

The current Copyright Office triennial video-game preservation exemptions are narrower and should not be treated as a generic emulator/recompiler/decryption publication license.

Counsel should also consider jurisdiction outside the U.S., contract/EULA facts, authorization, intended audience and whether publishing source vs binaries vs key-acquisition mechanisms changes the analysis.

## Verification evidence available to a reviewer

- `python tools/pgd_decrypt.py --selftest` checks AES against FIPS 197 without PSP constants.
- `tools/test_pgd_decrypt.py` uses synthetic data and gates real-file checks on private inputs.
- `tools/test_pgd_c.py` compiles the C implementation and compares it with the Python reference.
- `tools/test_pgd_hardening.py` / `tools/test_pgd_malformed.py` exercise malformed headers, overflow/size bounds, invalid keys/MACs, short reads and cache behavior with synthetic fixtures.
- `tools/publish_audit.py --tracked-only` checks the current tree for prohibited material; #102 separately owns history/privacy/proprietary-object review.

Correct decryption and tests prove engineering behavior, not permission to distribute source, binaries, keys or game content.

## Current distribution facts and unresolved questions

- The current private development tree includes both PGD implementations.
- The default development build embeds the C implementation even without local constants.
- [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) now defines a supported PGD-free source/build boundary. It excludes the runtime and standalone implementations and uses a fail-closed unavailable backend; the private development build remains unchanged.
- Current `main` excludes the fixed PSP constants, title key, encrypted retail file and decrypted retail output; old reachable history is not yet publication-clean.
- The recommended publication architecture is a **fresh sanitized public repository**, not a visibility flip of this historical private repository.

## Questions for counsel

1. Under the intended jurisdictions/facts, does publishing the derived-translated implementation without keys/constants create anti-circumvention/trafficking exposure?
2. Does distribution of a binary embedding the same unusable-without-local-values implementation materially change the analysis?
3. Does complete exclusion of PGD/amctrl from the initial generic public source materially reduce exposure, and is that the recommended first-release posture?
4. If source inclusion is acceptable, should it be a separate opt-in component, and what purpose/use/documentation language is appropriate?
5. Is the lawful-user interoperability purpose and user-supplied-input model legally material, and what contemporaneous development records should be retained?
6. What copyright/license notice or other source-treatment obligations follow from the recorded
   derived-translated PSP flow, mixed upstream family, independently expressed AES/hardening, and
   absence of a substantial near-verbatim body?
7. Are there jurisdictions, distribution channels or features the project should exclude?
8. What additional facts/source comparisons/release controls are required before source publication and before binary distribution?

## Required follow-through

Have counsel review the **actual candidate public tree/build configuration**. Preserve privileged legal advice privately; public GitHub issues/docs should record only the nonprivileged implementation decision, required notices and release constraints necessary for contributors/users to follow. Do not publish privileged attorney analysis, private keys or game-derived evidence.

No repository visibility or binary-release decision should be made from this packet alone.

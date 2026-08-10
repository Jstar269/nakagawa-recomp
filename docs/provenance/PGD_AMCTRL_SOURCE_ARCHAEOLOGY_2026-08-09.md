# PGD/amctrl source archaeology — 2026-08-09

**Status: technical provenance investigation complete to recoverable evidence.** This is an
engineering record for [issue #104](https://github.com/Jstar269/nakagawa-recomp/issues/104), not a
copyright, licensing, anti-circumvention, or distribution conclusion. Qualified review of the actual
candidate source and binary configurations remains open.

This report supersedes the maintained implementation-independence claims previously attached to
`tools/pgd_decrypt.py`, `src/rt/pgd.c`, and `src/rt/pgd.h`. The original 2026-07-18 statements remain
preserved in [`STATUS_HISTORY.md`](../STATUS_HISTORY.md) as contemporary development evidence.

No omitted PSP constant or title-key value is reproduced here. Named constants, service numbers,
format offsets, hashes of public/archive artifacts, and source revisions are used only to identify
provenance.

## Result

| Question | Engineering result | Confidence |
| --- | --- | --- |
| Was the Python implementation written before the C implementation? | **Yes, to high confidence.** The recovered private archive gives the Python file a 2026-07-18 09:42:52Z creation time, the first C header a 14:29:34Z creation time, and contemporaneous documentation/tests describe Python first and a later C port. | High |
| Is `src/rt/pgd.c` a port of `tools/pgd_decrypt.py`? | **Yes.** The function boundaries, operation order, comments, tests, and contemporary history all say so. | High |
| Is the PSP-specific BBMac/BBCipher/PGD flow independently organized? | **No longer claimed.** It is conservatively classified **derived-translated** from the public amctrl/PGD implementation family that the July 18 record says was consulted. | High |
| Is the local AES implementation copied from libkirk or another examined AES body? | **No evidence of that was found.** Its computed S-box/Rcon organization is independently expressed standard AES. | High |
| Are later validation, allocation, overflow, streaming, cleanup, and public-safe changes inherited? | **No substantial inherited expression was found.** They are project-specific defensive/runtime engineering. | High |
| Is any Nakagawa function body near-verbatim to an examined upstream body? | **No substantial near-verbatim body was found.** Distinct expression does not make the PSP flow independent when its staged logic was translated. | High |
| Does ProximaV/kirk-engine-full supply a high-level PGD ancestor? | **No.** It is first-class evidence for KIRK-core history only; no revision contains an amctrl, BBMac, BBCipher, or PGD layer. | High |
| Can the precise upstream file or line used for every Python block be recovered? | **No.** PPSSPP/libkirk consultation is contemporaneously documented, but the exact mix of PPSSPP, tpu, JPCSP, sign_np, secondary documentation, and prior knowledge is not recoverable. | Medium-high |

The result is deliberately mixed. “No near-verbatim body” and “local AES expression” are supported;
“clean-room,” “original implementation,” and “nothing copied” are not suitable maintained
descriptions of the complete PGD/amctrl unit.

## Scope and method

The investigation compared:

- Nakagawa `tools/pgd_decrypt.py`, `src/rt/pgd.c`, `src/rt/pgd.h`, their tests, first import,
  reachable history, unreachable objects, and recovered pre-Git/private snapshots;
- PPSSPP `ext/libkirk/amctrl.c` from its first import through current history;
- the pre-PPSSPP tpu/Fake_NP source package and the surviving tpunix `kirk_engine` Git import;
- JPCSP's pre-PPSSPP `CryptoEngine.java` and later split `AMCTRL.java`/`PGD.java` history;
- Draan's original KIRK-engine history, the surviving hexxellor mirror, and
  ProximaV/kirk-engine-full from its initial commit through current `master`;
- sign_np, uOFW, PRO CFW, psardumper, npdrm_free, and related surviving PSP tooling where relevant.

Comparison was expression-level, not a test for shared mathematics. It considered state
decomposition, helper boundaries, temporary buffers, BBMac finalization, version-key handling, KIRK
service order, BBCipher counter construction, header/MAC order, cleanup, names, comments, and the
Python-to-C mapping. Standard AES behavior and the externally required PSP service/format contract
were kept separate from expressive implementation lineage.

Limits:

- Filesystem timestamps are not authorship proof by themselves. Here they are corroborated by
  byte-identical Git blobs, test creation order, and a contemporaneous narrative.
- The July 13 object stores contain blobs and trees but no commits, reflogs, or author metadata.
- Public Git history does not recover tpu's complete Mercurial history or every older forum/package
  revision.
- Source similarity is evidence about expression and development sequence, not a legal test.

## Corrected Nakagawa chronology

### Before the GitHub root

| UTC time/evidence | Recoverable fact | Weight |
| --- | --- | --- |
| 2026-07-13 preserved object databases | The fullest retained object store has 629 blobs and 195 trees, but **zero commits**. No tree path names PGD/amctrl, and content scanning produced no recoverable implementation ancestor. | Strong negative evidence for those preserved snapshots; not proof about every deleted local file. |
| 2026-07-17 22:55:39–23:03:51 snapshot transition | A retained source copy was created at 22:55:39Z and its `src/` and `tools/` copy completed by 23:03:51Z. It has no `tools/pgd_decrypt.py`, `src/rt/pgd.h`, or `src/rt/pgd.c`. Later archive cleanup touched runtime directories, so absence is strongest for the Python file/header and less conclusive for a deleted C sketch. | High for “not present in the retained July 17 copy”; medium for excluding every abandoned C draft. |
| 2026-07-18 09:42:52.7185067 | Earliest exact surviving `tools/pgd_decrypt.py`; Git blob `5dc49b9ea8b8790fb4e8768a1aad7cb533d34d22`, identical to the root import. | High |
| 2026-07-18 14:29:34.1678500 | Earliest exact surviving `src/rt/pgd.h`; blob `1eca5b3cd49c6976edf1a51b673b99f1c77e2428`, identical to the root import. | High |
| 2026-07-18 18:49:46–18:51:17 | `docs/STATUS_HISTORY.md`, blob `b7429d1226908740d5b936a7d8a21fe0635b4081`, records the standalone implementation followed by a C port, while also recording a plan to port PPSSPP and later stating that libkirk was read as an algorithm reference. | High, contemporary self-report |
| 2026-07-18 19:12:29 | `tools/test_pgd_decrypt.py`, blob `a4dfbe87f22af9932944387dd2636e5c2a6d3fbe`, appears. | High |
| 2026-07-18 19:12:58 | `tools/test_pgd_c.py`, blob `38d47bc2c73645cf71f8bc135fbd1299e365bb0a`, appears and calls the C implementation a hand port of the Python implementation. | High |
| 2026-07-18 22:11:27 | Final pre-import modification time of the surviving Python file. | Corroborating |

No pre-root commit narrows the sequence further. The current object database has 493 unreachable
commits, but their earliest author and committer time is 2026-07-19 19:59:17-04:00—after the
parentless root import. The archived July 13 stores have no commit objects at all.

### Reachable Git history

| Date | Commit | Provenance significance |
| --- | --- | --- |
| 2026-07-19 | [`7ac90b25`](https://github.com/Jstar269/nakagawa-recomp/commit/7ac90b25ca0f5bc790df424eb54b7fdfdb0e2830) | Parentless “Moving to GitHub” import. It contains the exact recovered Python/header/test blobs and the earliest surviving C body, blob `1243d5aab01329268afc2b4f7fa30238e6f10dd5`. |
| 2026-07-19 | [`19610c9c`](https://github.com/Jstar269/nakagawa-recomp/commit/19610c9c6500fedcd490b9f4aa565a0ebff7ed2c) | Adds malformed-input, size, overflow, and cleanup hardening to the C path. |
| 2026-07-21 | [`ae5a40f5`](https://github.com/Jstar269/nakagawa-recomp/commit/ae5a40f595c19291b695b8e4a4ade9cacfce8223) | Adds the explicit block-size cap and related harness/sanitizer hardening. |
| 2026-07-22 | [`a2738e0f`](https://github.com/Jstar269/nakagawa-recomp/commit/a2738e0f0ac24df40cae488f7ec9926b09309804) | Externalizes PSP KIRK/amctrl platform data from the tracked tree. This changes data distribution, not algorithm ancestry. |
| 2026-07-24 | [`d4e8bcc1`](https://github.com/Jstar269/nakagawa-recomp/commit/d4e8bcc103ce77e4343fa648124a51ff9b2276d9) | Adds the original qualified-review packet. |
| 2026-07-31 | [`279370ef`](https://github.com/Jstar269/nakagawa-recomp/commit/279370ef9982c10487407ec1a95008bbbcdbe9e0) | Adds the `public-safe-v1` PGD exclusion and the independently expressed fail-closed API/backend seam. |
| 2026-08-04 | [`ac1f369f`](https://github.com/Jstar269/nakagawa-recomp/commit/ac1f369f3e4bd366a3ef7a0faaa4ef3f0c9cee77) | Reworks temporary keystream handling and further hardens finite-span behavior without changing the PSP flow. |

### Chronology conclusion

The earlier statement that pre-root chronology was wholly unrecoverable must be revised. It remains
true for the repository in general, but not for this unit: the private archive supports
**Python-first → C-port** to high confidence. What remains unrecoverable is any discarded draft before
09:42:52Z and the exact source-by-source consultation sequence inside the Python authoring session.
“Python first” describes the recoverable start/development sequence, not a claim that the Python file
stopped changing before C work began: its surviving file was modified later on July 18.

## Earliest recoverable public lineage

KIRK-core history and higher-level amctrl/PGD history are separate chains. A match to the required
KIRK service contract does not, by itself, establish ancestry of the surrounding PGD implementation.

### KIRK core

| Date | Source | Recoverable contribution |
| --- | --- | --- |
| 2011-01-06 | Draan/original KIRK engine, surviving mirror root [`d1fa7cd5`](https://github.com/hexxellor/kirk-engine/commit/d1fa7cd591cdfc46cf1b1c61044566d0076f7b68) | Earliest surviving Git KIRK engine. The commit says it mixes kgsws code and SilverSpring information. It contains KIRK-core command/key handling, not amctrl or PGD. |
| 2011-01-29 | [`8cfe6217`](https://github.com/hexxellor/kirk-engine/commit/8cfe6217eb588fb2cdb5bf8ba541bcb23f6ed6ec) | Fixes command 4 and adds a GPLv3 license text. Earlier commits did not contain that repository license file. |
| 2011-01-30–2012-11-07 | [`84526840`](https://github.com/hexxellor/kirk-engine/commit/8452684014542640f0353d0a9d81a5300907129e) through [`7704109e`](https://github.com/hexxellor/kirk-engine/commit/7704109e6b76676743c4bf795f6b9ff424c73cfe) | Creates `libkirk`, adds IPL/ECDSA/SHA-1 work, and expands the service/key implementation. Still no high-level amctrl/PGD layer. |
| 2011 onward | PRO CFW and other PSP tools | Surviving PRO CFW paths incorporate KIRK-engine cores for PRX/install tooling. No earlier high-level BBMac/BBCipher/PGD expression was found there. |
| 2011-10 onward | uOFW | Exposes amctrl/NPDRM interfaces and firmware-library artifacts, but no recoverable high-level C implementation predating the tpu/JPCSP sources below. |

### BBMac, BBCipher, and PGD

| Date | Source | Recoverable contribution |
| --- | --- | --- |
| 2011-01-23–26 | tpu, Fake_NP source package ([surviving forum attachment](https://endlessparadigm.com/forum/showthread.php?tid=25799)) | Earliest recovered C `amctrl.c`/`.h`. It has the `MAC_KEY`/`CIPHER_KEY` split, shared `kirk_buf`, KIRK wrapper helpers, BBMac stages, BBCipher temporary-key/counter stages, and named amctrl data. Archive SHA-256: `b65a17da0a6353ef7723ddc50e8d6f24f9cb10ab66e33feeb603e2b589a83f5c`. No project-level reusable-license grant was found in that package. |
| 2011-03-07 | JPCSP [`5d41d35d`](https://github.com/jpcsp/jpcsp/commit/5d41d35d900ae2d44355f17295d653c535d0ff16) | Earliest recovered implementation combining amctrl behavior with PGD decryption in a public VCS. It adds BBMac/BBCipher contexts and PGD parsing/decryption to `CryptoEngine.java`. |
| 2013-02-24 | PPSSPP [`a4f65624`](https://github.com/hrydgard/ppsspp/commit/a4f65624c3771708716e1ff5dcb40b586341c182) | tpu-authored import of `ext/libkirk/amctrl.c/.h` and PGD I/O integration. The C file includes both amctrl and `pgd_open`/block decryption. |
| 2013-03-03 | tpunix [`a29c3991`](https://github.com/tpunix/kirk_engine/commit/a29c3991b34a1f8f05c256b0fc3c8f56299268dd) | Git import from tpu's earlier Mercurial tree. It preserves and extends `kirk/amctrl.c` and separates higher-level `npdrm/pgd.c`. The one-week Git ordering does not make PPSSPP the origin; both point to tpu's pre-Git/Hg work. |
| 2013-10-05 | JPCSP [`3858c680`](https://github.com/jpcsp/jpcsp/commit/3858c680a607a659fd5d35a2fec906fd08897248) | Splits the older `CryptoEngine` implementation into `AMCTRL.java`, `PGD.java`, and other crypto modules. |
| 2015-01-14/17 | sign_np [`478dd112`](https://github.com/swarzesherz/sign_np/commit/478dd112b714b5007960619eb856ece86d535de2), [`ac77d56e`](https://github.com/swarzesherz/sign_np/commit/ac77d56e13b5c6c60564100699560d12ff3f425f) | Imports an amctrl implementation, then adds its PGD implementation and large-file handling. The repository contains GPLv3 license text. |
| 2018 onward | npdrm_free and later tools | Later PGD consumers/descendants; no earlier expressive ancestor was found. |

PPSSPP's full `amctrl.c` history begins at `a4f65624`, then adds DLC support at
[`641b78ab`](https://github.com/hrydgard/ppsspp/commit/641b78ab6765c223cf3ea96c2f46572cf0687b64),
receives later libkirk/PRX and cleanup changes, and removes global KIRK state at
[`c29e370e`](https://github.com/hrydgard/ppsspp/commit/c29e370e292b6620ede735e4fc8841fbf263fc1b).
The earliest Git commit is therefore not the earliest public source package, and PPSSPP's current
copy is not an adequate substitute for the tpu/JPCSP history.

## ProximaV/kirk-engine-full as first-class KIRK evidence

Repository: [ProximaV/kirk-engine-full](https://github.com/ProximaV/kirk-engine-full). Remote
`master` was rechecked at `3eb9bf14108215612f12e47907f9bb6a0c16394a` for this report.

### Generation 0: repository shell

[`55618122`](https://github.com/ProximaV/kirk-engine-full/commit/5561812233f81e7c4b4fdc76c89a2b6c783d8c7a)
(2020-01-11 11:33:52-06:00) contains only a README describing the project as an update to the
original KIRK engine. It contains no implementation.

### Generation 1: inherited import

[`883f992c`](https://github.com/ProximaV/kirk-engine-full/commit/883f992c02f12639d66be64c42f9771ab5f93691)
(five minutes later) imports the implementation and tests.

- `AES.c`, `AES.h`, `SHA1.c`, `SHA1.h`, `bn.c`, `ec.c`, and `license.txt` have exact Git-blob
  identity with the final surviving Draan/hexxellor tree at `7704109e`.
- Most common `kirk_engine.c` functions preserve the original libkirk organization and normalized
  bodies. The import is inherited KIRK-engine expression, not a new Proxima implementation.
- The imported `kirk_4_7_get_key` generation is closer to PPSSPP's expanded early-2020 libkirk copy
  than to the final Draan-mirror body. That mapping predates Proxima's later discoveries.
- No amctrl, BBMac, BBCipher, or PGD file is present.

### Generation 2: Proxima additions and refactors

| Commit | Change attributable to the later Proxima branch |
| --- | --- |
| [`4c94a975`](https://github.com/ProximaV/kirk-engine-full/commit/4c94a975b8676670810baf5aea45f0689ed38d13) | Starts services 5/6/8/9 support. |
| [`b5cfa4cd`](https://github.com/ProximaV/kirk-engine-full/commit/b5cfa4cd4e1db3f71689a0504ff1e1378f49d32a) | Corrects 6/9 output formatting. |
| [`30d62f94`](https://github.com/ProximaV/kirk-engine-full/commit/30d62f942474b659c8004e6d44206bfde155e6cd) | Makes/tests the 5–8 service generation. |
| [`37e79610`](https://github.com/ProximaV/kirk-engine-full/commit/37e7961003ed98eb06eaf43f204ed2245be1b672) | Adds service 3/0 and encryption-side service 1/3 helpers. |
| [`238753da`](https://github.com/ProximaV/kirk-engine-full/commit/238753dae6753388f9eba807aa09dea6ce411acb) | Adds service 3 test coverage. |
| [`0826aebe`](https://github.com/ProximaV/kirk-engine-full/commit/0826aebefd69c1edad3c1373dcf161860477c684) | Corrects service 0 key-slot use. |
| [`56cad494`](https://github.com/ProximaV/kirk-engine-full/commit/56cad494d9107ea1db3069343983bdf00db3fd91) | Updates the project description. |
| [`101ccc25`](https://github.com/ProximaV/kirk-engine-full/commit/101ccc251b3f41a3a1f21715bf1d78e4a4d2135f) | Adds service 18 certificate verification. |
| [`8da1e55a`](https://github.com/ProximaV/kirk-engine-full/commit/8da1e55a695a6ada0bf6e7c2f05b5bb3f5b7b1e4) | Fixes KIRK16 key-generation behavior. |
| [`3a9ad20b`](https://github.com/ProximaV/kirk-engine-full/commit/3a9ad20b994cc225d49c360224dfa192c549b014) | Re-enables service 1/3 CMAC data checks. |
| [`217627e9`](https://github.com/ProximaV/kirk-engine-full/commit/217627e959bc68b94dc51f12b7b83f87e8b5ed92) | Refactors size handling, formatting, and PRNG sizing. |
| [`5fbc3df1`](https://github.com/ProximaV/kirk-engine-full/commit/5fbc3df193cbbe6fd2ce9db274893cd06a3dadbf) | Updates elliptic-curve implementation material. |
| [`88094f5a`](https://github.com/ProximaV/kirk-engine-full/commit/88094f5af9fa7c2127c2620e0a3abc6ea6a544b1) | Fixes `kirk_init2()` random-seed handling. |
| [`c2d40656`](https://github.com/ProximaV/kirk-engine-full/commit/c2d40656da29a66e222a2e2d5376f63d09aa50fd), [`64c8aad8`](https://github.com/ProximaV/kirk-engine-full/commit/64c8aad8d0de5ba45dc6fe2f50583723aab9ea77), [`3eb9bf14`](https://github.com/ProximaV/kirk-engine-full/commit/3eb9bf14108215612f12e47907f9bb6a0c16394a) | Replaces the older `bn.c`/`ec.c` arrangement with `ecdsa.c/.h`, accelerates ECDSA, and updates AES declarations. |

At current `master`, inherited AES/SHA-1 source remains substantially recognizable while
`kirk_engine.c/.h`, ECDSA organization, service coverage, validation, and tests carry extensive later
Proxima work. This dates service 5/6/8/9, service 0/3 encryption support, service 18, mesh/KIRK16,
PRNG-size, and ECDSA refactors to Proxima's 2024–2025 branch. It does **not** date the higher-level
amctrl sequence, temporary `kirk_buf` organization, BBMac/BBCipher state machines, or PGD header
order; those are present in the older tpu/JPCSP lineage and absent from every Proxima revision.

## Function/block provenance matrix

The classification vocabulary below is the one requested for this investigation. “Translated” does
not mean literal copying; it records that substantial PSP-specific staged logic was carried into a
new expression after consultation of prior implementations.

### Python implementation

| Nakagawa unit | Expression-level evidence | Classification | Confidence |
| --- | --- | --- | --- |
| `_gf_mul`, `_gf_inv`, `_rotl8`, `_key_expansion`, AES round helpers | Table-free, locally decomposed AES using the published field/round definition; unlike the examined libkirk table/helper bodies. | **Standard cryptographic primitive / externally required fact**, independently expressed | High |
| Key-file parser, `_KEYSEED_NAMES`, `_k`, `_keyvault` | Project-specific externalization/schema; the named platform data and service selection are externally required PSP facts. | **Independently expressed PSP behavior** for the loader; **externally required fact/data** for names/selections | High |
| `kirk4`, `kirk7` | Thin fixed-key CBC service wrappers. Service number, IV, and key-slot selection are dictated by the KIRK contract; helper expression is local. | **Structurally corresponding but plausibly dictated by protocol** | High |
| `_cmac_shift` | Standard CMAC doubling operation. | **Standard cryptographic primitive / externally required fact** | High |
| `_bbmac_cbc`, `bbmac`, `bbmac_verify` | Reorganizes the earlier streaming `MAC_KEY` API into whole-buffer helpers, but retains final-block holdback, subkey derivation, fixed-data mixing, optional version-key stage, and type-dependent verification sequence. | **Translated/adapted expression** | High |
| `bbcipher_decrypt` | Reorganizes the earlier `CIPHER_KEY`/global-buffer implementation, but retains the same header-key/version-key combination, two named mixing stages, seeded counter blocks, service sequence, first-block correction, chunking, and XOR order. | **Translated/adapted expression** | High |
| `Pgd.__init__` | The magic/field selection, fixed-key restriction, MAC-at-0x80 then MAC-at-0x70 order, parameter-block decryption, and extracted fields map directly to the public PGD family. | **Translated/adapted expression** | High |
| `Pgd.decrypt_block` | Block offset to seed conversion and BBCipher use are protocol behavior expressed through the translated helper. | **Structurally corresponding but plausibly dictated by protocol** | High |
| `PgdError`, CLI, self-test, local-output handling | Project-specific interface, diagnostics, and FIPS known-answer gate. | **Independently expressed PSP behavior / standard primitive test** | High |

### C implementation

| Nakagawa unit | Expression-level evidence | Classification | Confidence |
| --- | --- | --- | --- |
| `gf_mul` through `aes_cbc_decrypt0` | Direct C port of the Python AES organization; computed tables and local byte-oriented rounds, not an examined libkirk body. | **Standard cryptographic primitive / externally required fact**, independently expressed | High |
| `pgd_keys_parse`, `kirk_init`, availability/path API | Added during key externalization; defensive parsing and fail-closed state are project-specific. Named data remain separate PSP platform facts. | **Independently expressed PSP behavior** | High |
| `cmac_shift`, `bbmac_cbc`, `bbmac`, `bbmac_verify` | Close structural port of the Python units, which themselves translate the earlier amctrl staged flow. Constant-space CBC later replaces a temporary allocation. | **Translated/adapted expression** for PSP flow; later storage hardening independently expressed | High |
| `bbcipher_tmp2`, `bbcipher_apply` | Close Python-to-C mapping of the PSP-specific derivation/counter flow. The later one-block-at-a-time CBC implementation is local hardening. | **Translated/adapted expression** with independently expressed hardening | High |
| `SrPgd` basic fields and `sr_pgd_open` protocol stages | Basic decrypted-key/size fields and validation order correspond to earlier PGD descriptors/open functions; Python-to-C mapping is direct. | **Translated/adapted expression** | High |
| `pgd_validate_sizes`, checked seek arithmetic, allocation/error cleanup | Absent from the early public PGD bodies in this form; added in traceable post-import hardening commits. | **Independently expressed PSP behavior / defensive engineering** | High |
| Cache/scratch split and `sr_pgd_block` read integration | Runtime-specific on-demand block cache. The seed/decrypt operation is protocol-derived; cache validity, bounded reads, and failure cleanup are local. | **Independently expressed runtime organization** plus **translated protocol step** | High |
| `pgd_api.h`, `pgd_unavailable.c` | Added for `public-safe-v1`; no upstream PGD implementation ancestry found. | **Independently expressed PSP behavior / project policy seam** | High |

No unit is classified **near-verbatim/copy lineage**. The **unknown** category applies only to the
unrecoverable exact source-consultation mix and any discarded pre-snapshot draft, not to the current
file-level classification.

### Cross-generation expression details

| Dimension | Earlier public family | Nakagawa Python/C | Provenance weight |
| --- | --- | --- | --- |
| State decomposition | Firmware-like streaming `MAC_KEY` and `CIPHER_KEY` contexts, plus a shared KIRK work buffer in the tpu C line | Whole-buffer Python helpers; reduced fixed-path C helpers and a runtime `SrPgd` cache | Different organization, but not enough to overcome the documented translation of the staged flow |
| Temporary buffers | Global work buffer and offset/header staging in tpu/PPSSPP; local arrays in JPCSP/sign_np variants | Python immutable byte sequences; C fixed local arrays, then constant-space streaming after hardening | Strong source-shape difference; later C storage policy is independently expressed |
| BBMac subkey/padding | Update holds back the last block; final derives/doubles the AES-zero block and mixes the retained block | `body, last`/`running` in Python and `body`/`running` in C preserve the same decomposition | Derived-translated; CMAC doubling itself is standard |
| Version-key handling | Optional version key is XORed into the intermediate MAC and followed by the same encryption stage | Same stage in `bbmac`; different API and local variable organization | Derived-translated |
| KIRK service sequence | amctrl wrappers select the required encrypt/decrypt service and key slot | Direct AES-CBC helpers named `kirk4`/`kirk7`; C expands only the required local keys | Contract is externally required; reduced helper organization is local |
| BBCipher temporary key | Header/file key combines with the first mixing datum, the required service runs, then the second mixing datum is applied | `bbcipher_decrypt`/`bbcipher_tmp2` preserve that order | Derived-translated |
| Seed/counter construction | 12-byte temporary-key prefix plus little-endian counter, with a prior-block/first-block correction and bounded chunks | Same construction in Python; direct C port, later emitted one block at a time | Derived-translated flow; streaming storage hardening independent |
| Block processing order | Initialize cipher state, construct/decrypt keystream, correct first block, XOR payload | Same order, flattened into a symmetric XOR helper | Derived-translated |
| PGD header order | Select types/fixed key; verify the 0x80 MAC; verify the 0x70 MAC/version key; decrypt parameters; derive block key | Same order in `Pgd.__init__` and `sr_pgd_open` | Some order is protocol-required, but the documented consultation plus full sequence supports translation |
| Errors and cleanup | Early bodies allocate from decrypted sizes and have firmware/tool-style integer returns and shared scratch | Exceptions in Python; NULL/fail-closed C path, checked arithmetic, caps, short-read handling, cache validity, centralized free | Independently expressed defensive/runtime hardening |
| Names/constants | `loc_*`, `tmp1`/`tmp2`, service/key-slot names occur across decompiled/public sources | Some named data and `tmp2` concepts remain; public function boundaries/names are otherwise different | Names of platform data are factual/decompiler lineage; their use helps map the translated flow but is not near-verbatim proof |
| Comments | tpu/PPSSPP/sign_np retain firmware/reverse-engineering and `sub_*` commentary | Nakagawa explains whole-buffer behavior, verification, and hardening | Normalized substantive-comment comparison found no exact intersection with the three pinned C amctrl files |
| Python → C | Not applicable | AES, BBMac, BBCipher, PGD-open, and block-decrypt boundaries correspond directly and `test_pgd_c.py` calls the C file a hand port | Direct translated/adapted expression within Nakagawa; high confidence |

No shared global-work-buffer organization, streaming context structure, `sub_*` helper set, or
substantial comment/body block survived near-verbatim into Nakagawa. Conversely, the full combination
of service stages and ordering is too specific—and the consultation record too direct—to describe the
high-level result as merely independent protocol convergence.

## Why required behavior is not enough—and why derivation is still the result

Some correspondence is inevitable:

- AES round behavior and CMAC doubling are standard cryptographic operations.
- KIRK command numbers, key-slot selection, CBC direction, PGD field offsets, and block seed meaning
  are external protocol/platform facts.
- A conforming PGD implementation must eventually verify the appropriate MACs and decrypt the
  parameter/data blocks.

The stronger ancestry signal is the *combination* of the July 18 consultation record and the retained
multi-stage organization: final-block BBMac handling, version-key placement, type-dependent final
verification, the same BBCipher temporary-key stages, the same 12-byte-prefix/little-endian counter
construction, the same first-block correction, and the same PGD MAC/decrypt sequence. Nakagawa
compresses firmware-style state machines into whole-buffer helpers and uses different names/local
storage, but it carries substantial staged logic from the consulted implementation family. That is
why the high-level flow is derived-translated even though no body is near-verbatim.

## Constants and data

| Category | Provenance treatment |
| --- | --- |
| Title-specific version key | Private title input. It is not implementation expression and is not distributed. |
| PSP KIRK/amctrl platform data | Platform/service data published across the KIRK/amctrl lineage. Exact public discovery/extraction history is incomplete; current values are supplied locally and omitted here. Data provenance and implementation-expression provenance are separate. |
| Standard AES constants/field facts | Published cryptographic standard facts. Nakagawa computes rather than transcribes lookup tables. |
| Algorithms/service sequence | Required behavior has factual elements, but Nakagawa's complete PSP-specific staged flow was implemented after consulting prior implementations and is classified derived-translated. |
| Loader/schema/error policy | Project-specific expression introduced by externalization and hardening. |

Calling all KIRK/amctrl data “public facts” is too broad if it is read as a conclusion about
redistribution or acquisition. The maintained wording should say **PSP platform data with public
historical implementations, supplied locally and separately reviewed**. It should not conflate those
values with the private title key or with source-code expression.

## Claim disposition

| Maintained/historical claim | Disposition |
| --- | --- |
| Python was written first, then ported to C | **Supported, high confidence** by recovered timestamps/blobs plus contemporary tests/history. |
| AES was implemented locally from the field definition | **Supported, high confidence** for the examined current/root bodies. |
| Entire PGD/amctrl implementation is “original,” “clean-room,” or “nothing copied” | **Contradicted/overstated** as a maintained independence claim. Preserve only as dated historical self-report. |
| PPSSPP/libkirk, kirk_engine, and sign_np were only behavioral references | **Overstated** for the high-level PSP flow; conservative classification is derived-translated. **Supported only for Proxima's separate KIRK-core role.** |
| No substantial near-verbatim Nakagawa body exists | **Supported, high confidence** for the examined corpus and normalized/source-shape comparisons. |
| Exact single implementation ancestor is known | **Unknowable from surviving evidence.** The family and documented PPSSPP/libkirk consultation are known; per-block source contribution is not. |
| Platform constants, title keys, AES, and source expression share one provenance question | **Contradicted.** They are four separate categories and must stay separate. |

## Engineering publication boundaries

This table identifies which *technical/provenance uncertainties* disappear at each boundary. It is
not a distribution recommendation or legal conclusion.

| Boundary | What is absent/resolved technically | What remains |
| --- | --- | --- |
| PGD completely excluded (`public-safe-v1`) | No PGD/amctrl implementation expression, platform data, title key, or binary implementation is shipped in that profile. Source-lineage and implementation-distribution questions do not apply to that candidate tree. | Qualified confirmation of the actual candidate; old-history sanitation; any future retaining configuration; general project obligations. |
| Source present, not compiled/default-disabled | Removes default-binary inclusion and runtime-use questions. | Source-expression lineage, applicable notices/licenses, source-distribution and anti-circumvention questions, and platform-data documentation boundaries. |
| Source/binary present, no keys/constants | Resolves only distribution of the omitted data/title key. | Derived implementation lineage, source/binary distribution treatment, and whether a keyless binary materially changes review. |
| Independently reimplemented replacement | If built from an independently controlled specification/hardware-evidence process, can remove inherited expressive implementation uncertainty for the replacement. | Required PSP behavior/platform data, the process record, correctness, and qualified distribution review still remain. Prior exposure cannot be retroactively called clean-room. |
| Retain current implementation | Preserves proven behavior and hardening. | Must accurately disclose derived-translated flow and resolve the qualified licensing/legal/anti-circumvention treatment for each retaining configuration. |

Conservative engineering decision tree:

1. Keep `public-safe-v1` exclusion as the supported initial boundary while #104's qualified review is
   open.
2. If a retaining source or binary configuration is approved and its notice/license treatment is
   established, retain the current implementation with the mixed provenance disclosure.
3. If retaining treatment cannot be established or the maintainer wants a narrower ancestry surface,
   replace the high-level flow under the documented independence model; do not relabel the current
   implementation.

## Smallest remaining factual unknowns

1. Which exact public file(s), revisions, or secondary notes were open while each Python block was
   authored. PPSSPP/libkirk consultation is documented, but the detailed mix is not.
2. Whether an abandoned C sketch existed and was deleted before the retained July 17→18 transition.
   No surviving blob/tree supports one.
3. The complete pre-Git Mercurial history of tpu's `kirk_engine`, and the source/license status of the
   older “NP Decryptor” credited by Fake_NP.
4. The exact discovery/publication chain for each PSP platform datum. This is separate from current
   source expression and no value is needed to continue the archaeology.
5. The qualified conclusion about applicable copyright/license notices and anti-circumvention or
   other distribution rules for each candidate configuration.

Additional public-repository and archive searching stopped because it no longer narrowed the current
implementation ancestry: every newly located branch either joined the established tpu/JPCSP family,
postdated it, or contained only KIRK-core/service material.

## Completion boundary

The **technical provenance investigation is complete to recoverable evidence**. The earlier broad
“pre-root chronology unrecoverable” statement is superseded for PGD, the Python-first/C-port sequence
is high-confidence, the high-level flow is classified derived-translated, and Proxima's contribution
is bounded to KIRK-core history.

This does **not** complete #104. Qualified human licensing/legal/anti-circumvention review of the
actual retain/exclude/replace configuration remains a separate open acceptance criterion.

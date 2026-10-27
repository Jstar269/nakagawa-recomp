# Security policy

## Supported versions

The unreleased `main` branch is currently the only supported development line. There are no supported
release versions yet, and the project does not claim a release-grade security posture: treat arbitrary
PSP and game inputs as untrusted until the parser and guest-span hardening work tracked in GitHub Issues
is complete.

| Version | Supported | Status / Support Policy |
| :--- | :--- | :--- |
| `main` | :white_check_mark: | Active development line. Fixes for all security reports land here. |
| `< v0.0.1` | :x: | No earlier release has been published. |
| `v0.0.1` | :hourglass: | Planned first experimental platform release ([#278](https://github.com/Jstar269/nakagawa-recomp/issues/278)); not yet published. |

### Release policy transition (v0.0.1)

When the first formal experimental platform release (`v0.0.1`) ships:

- The `main` branch remains the primary supported development line.
- `v0.0.1` is an experimental prerelease milestone, not an LTS or patch-supported line: fixes for security issues identified in `v0.0.1` land on `main` and flow into subsequent releases rather than receiving backported security point releases.
- A release stops receiving security updates as soon as the next release is published; move to the newest
  release or to `main` to keep receiving fixes.
- Release qualification authority [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278) owns updating this policy and marking `v0.0.1` active atomically at release publication time. No release has been published.

## Report a vulnerability

Do not open a public issue for a vulnerability, and do not attach proprietary game inputs, private
paths, secrets, or exploit data to a public thread.

Report privately with GitHub's **Report a vulnerability** button on the repository's
[Security tab](https://github.com/Jstar269/nakagawa-recomp/security). If that button is ever
unavailable, contact the repository owner through the private contact method on their GitHub profile
and ask for a secure channel before sending details.

Include the affected revision, impact, reproduction conditions, relevant logs with personal and game
data removed, and any proposed mitigation.

Rights holders with a concern about any file in this repository can use the same private channel; see
[NOTICE.md](NOTICE.md#legal-and-contact).

## How reports are handled

1. **Acknowledge** the report within 14 days, in the private advisory thread.
2. **Triage**: reproduce it, decide whether it is in scope (below) and rate its severity.
3. **Fix** in a private fork of the advisory, with a regression test that fails before the fix.
4. **Disclose**: merge the fix, then publish the GitHub security advisory (requesting a CVE when the issue warrants
   one) and note it in the release notes of the next release.
5. **Credit**: reporters are named in the advisory and release notes unless they ask to stay anonymous.

This is a volunteer project, so remediation time depends on severity and complexity; the reporter is kept informed
in the advisory thread throughout.

## Published vulnerability data

Fixed vulnerabilities are published as GitHub security advisories at
<https://github.com/Jstar269/nakagawa-recomp/security/advisories>, with affected and fixed revisions. When a
known vulnerability in a dependency does not affect this project, the dismissal records why (an OpenVEX-style
`not_affected` justification such as "vulnerable code not present" or "not reachable"), and the advisory or
dependency alert links it, so users can audit the decision.

## Secrets and credentials

- **Where they live.** Credentials the project uses exist only as encrypted GitHub Actions secrets or in the
  maintainer's local credential store, never in the repository. Secret scanning with push protection and the
  publication audit block a committed secret.
- **What exists.** Workflows use the per-run `GITHUB_TOKEN` (read-only by default; jobs request only the extra
  scopes they need) and one fine-grained token used only by the provenance workflows, which never run
  pull-request code.
- **Who can access them.** Only the maintainer (see [Governance and roles](CONTRIBUTING.md#governance-and-roles)).
- **Rotation.** Tokens are created with an expiry of at most one year and rotated before it, and immediately
  when exposure is suspected or a person with access leaves.

## Scope

In scope:

- memory safety and guest-to-host boundary issues in the runtime (`src/rt/`), including HLE handlers
  that read or write guest pointers;
- unsafe processing of malformed ELF/PRX/ISO/XB/PSMF or save inputs by the runtime, the Python
  translation tools (`tools/`), or the native player and its core library (`src/player/`, `src/core/`);
- path traversal or unsafe filesystem access in the VFS, extraction and staging tools, the native
  player;
- host-side hardware-runner and oracle tooling (`tools/psp_oracle/`) that talks to a connected device;
- publication and provenance controls that could let private inputs, paths, keys, or traces reach the
  public tree, an export, or a CI log; and
- GitHub Actions workflows, dependencies, and release-packaging issues directly affecting this
  repository.

Out of scope:

- vulnerabilities in the original game, PSP firmware, or third-party products when used independently;
- compatibility bugs without a security impact; and
- reports that require redistribution of copyrighted game data.

This research software processes untrusted binary formats and is not sandboxed. Run it only on files
you are authorized to use.

## Encrypted PSP files

The player decrypts the encrypted executable and modules of a disc the user owns, locally, with built-in game tables
(`src/core/nk_psp_*`, [#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). It never reads a key file and
never decrypts firmware: firmware containers are refused by name before any key is read. Every container is
size-bounded, integrity-checked (CMAC, SHA-1 or ECDSA) before its payload is used, and each output is validated as
ELF32/MIPS before it is kept; one it cannot decrypt is refused by name. Every file is treated as untrusted input by the bounds-checked, fuzzed parsers listed in
[`docs/PARSER_SAFETY_INVENTORY.json`](docs/PARSER_SAFETY_INVENTORY.json).

## Digest inventory

Every digest the project's own code computes is classified here by what it is for, tracked in
[issue #969](https://github.com/Jstar269/nakagawa-recomp/issues/969). PSP-defined algorithms, Git object
identities and format checksums are never replaced. Project-controlled integrity checks use SHA-256, the
project default: provenance, attestation, packages, prerequisites, toolchain, SBOM, fonts, shaders and the
VFPU tables at load all rest on it, so SHA-256 is classified as a whole and has no rows below.

Every other use is one row. `python -m unittest tools.test_lint_docs` scans the tracked production files
under `tools/` and `src/` (test files are oracles and are skipped) for `hashlib` calls other than SHA-256,
`zlib` CRC-32 and Adler-32, and hand-rolled FNV and CRC-32 helpers, found by their primes and polynomials.
It fails when a file and algorithm pair is missing here or a row names a pair the scan no longer finds.

| File | Algorithm | Purpose | Disposition |
| --- | --- | --- | --- |
| `tools/decompme_export.py` | SHA-1 | PSP NID derivation (`sha1(name)[0:4]`, little-endian) | Keep: the PSP defines it |
| `tools/gen_nidnames.py` | SHA-1 | PSP NID derivation | Keep: the PSP defines it |
| `tools/nid_name_proof.py` | SHA-1 | PSP NID derivation | Keep: the PSP defines it |
| `tools/verify_vfpu_provenance.py` | SHA-1 | Git blob ID of a VFPU table against the pinned upstream tree | Keep: Git object identity |
| `tools/validate_assets.py` | MD5 | Reads legacy, unversioned local asset references; new ones are versioned SHA-256 | Keep: legacy compatibility only, not a security check |
| `tools/extract_xb.py` | CRC-32 | PNG chunk checksum | Keep: the format requires it |
| `tools/nk_cli.py` | CRC-32 | PNG chunk checksum | Keep: the format requires it |
| `tools/ppm2png.py` | CRC-32 | PNG chunk checksum | Keep: the format requires it |
| `tools/stage_install.py` | CRC-32 | PNG chunk checksum | Keep: the format requires it |
| `src/core/nk_psp_inflate.c` | CRC-32 | gzip member trailer of a decrypted PSP payload (RFC 1952) | Keep: the format requires it |
| `tools/codegen.py` | FNV-64 | Staged module identity (`fnv1a64`), mirrored by `src/rt/guest_interp.c` | Candidate: SHA-256 only with a codegen and package identity bump |
| `src/rt/guest_interp.c` | FNV-64 | Staged module identity (`sr_module_code_fnv1a64`), checked against the staged file | Candidate: SHA-256 only with a codegen and package identity bump |
| `tools/codegen.py` | FNV-32 | Self-modifying-code block record (`stale_fnv1a`), mirrored by `src/rt/stale_code.c` | Keep: change detector |
| `src/rt/stale_code.c` | FNV-32 | Self-modifying-code detection (`sr_stale_fnv1a`), checked per block | Keep: performance-critical change detector |
| `src/rt/mpeg.c` | FNV-64 | Change detector for a cached YCbCr conversion buffer | Keep: cache validity, not integrity |
| `src/rt/ge.c` | FNV-32 | GE transition trace draw identity and watched-buffer hash | Keep: trace key, not integrity |
| `src/rt/gpu_sdl3vk/ge_gpu.c` | FNV-64 | Texture cache key | Keep: cache key, not integrity |
| `src/core/nk_xb.c` | FNV-32 | In-memory path table that finds duplicate paths while writing an XB archive | Keep: hash-table key |
| `src/player/main.c` | FNV-64 | Rendered-frame hash printed by the player's UI test mode | Keep: test fingerprint |
| `src/ui/backend/nk_ui_sdlr.c` | FNV-32 | Glyph atlas cache key | Keep: cache key, not integrity |
| `src/ui/nk_ui_widgets.c` | FNV-32 | Picks a placeholder art tint from a key | Keep: not a digest check |

Outside the scan: the vendored FFmpeg subset compiles its byte-identical `libavutil/sha.c`
([`src/rt/h264_ffmpeg_sub/PROVENANCE.md`](src/rt/h264_ffmpeg_sub/PROVENANCE.md)), used only by its own
`random_seed.c`; the NIDs of the PSP's own MD5 and SHA-1 functions and the `eap_md5` registry key are guest-visible API names, not
host digests; and provenance tools use Git's own object and commit IDs.

## Assurance case

Why the project's security requirements are met, in brief:

- **Threat model.** The attacker controls the files a user feeds in (ISO, ELF/PRX, archives, saves, fonts) or tries
  to slip a change into the repository or its supply chain. The project holds no user accounts, network services or
  secrets of its own; a user's game files stay on their machine.
- **Trust boundaries.** (1) User files → the parsers in `src/core/`, `src/player/`, `src/rt/` and `tools/`.
  (2) Guest code → host memory, crossed only through HLE handlers that validate every guest pointer and size.
  (3) Contributions → `main`, crossed only through reviewed pull requests with required CI.
  (4) Private inputs → the public tree, crossed only through the publication and provenance gates.
- **Secure design principles applied.** Fail closed: unsupported behaviour stops with a named reason instead of
  guessing. Complete mediation: every guest-to-host access is checked against the guest span. Least privilege:
  workflows default to read-only tokens and pin every action by commit. Economy of mechanism: one runtime,
  one dispatch path, no permissive switches. Defence in depth: a file that passes one check is still
  bounds-checked by every later parser.
- **Common weaknesses countered.** Memory errors (CWE-119/787/125): bounds-checked parsers with hostile-input tests
  ([`docs/PARSER_SAFETY_INVENTORY.json`](docs/PARSER_SAFETY_INVENTORY.json)), ASan/UBSan fuzzing (ClusterFuzzLite)
  and CodeQL on every change. Path traversal (CWE-22): the VFS and extraction tools refuse paths outside their roots.
  Integer overflow (CWE-190): checked size arithmetic before forming host pointers. Supply chain: hash-pinned Python
  dependencies, pinned toolchain downloads verified by SHA-256, Dependabot, dependency review, OSV-Scanner and
  OpenSSF Scorecard. Leaked secrets: secret scanning and a publication audit on every push.

# Notices, provenance, and disclosures

The repository-level project declaration is GPL-3.0-or-later; the canonical text
is in [LICENSE](LICENSE). Individual files retain their own SPDX identifiers and
upstream terms. This notice records engineering provenance and required notices;
it is not legal advice or a conclusion that any combined distribution is cleared.

## Upstream families

- **sal063 PSP Recompilation Project** — GPL-2.0-or-later, public revision
  `da17b0e1db209206a407d097d132201e516e3855`. Nakagawa began from this toolkit
  and retains substantial modified runtime and pipeline code. See the
  path-hashed [public provenance ledger](assets/public_provenance_ledger.json).
- **PPSSPP** — GPL-2.0-or-later, with its own third-party notices. Several HLE,
  GE, VFPU and recompiler structures are translated or adapted through the
  sal063 lineage. PPSSPP-origin data is retained only where the profile records
  its exact path, revision and notice.
- **PSPSDK / PSPDEV** — permissive per-component declarations and BSD-family
  notices for public ABI definitions and source-owned fixtures. The applicable
  notices are in `THIRD_PARTY_LICENSES/PSPSDK.txt` and `assets/upstream/`.
- **FFmpeg n4.4 ATRAC3+ subset** — LGPL-2.1-or-later. The imported decoder and
  its license/provenance file are under `src/rt/atrac3p/`; the project wrapper
  remains separately identified in the ledger.
- **SDL3** — zlib; used by the optional host renderer/audio integration. The
  first public profile excludes the sal063-derived `src/rt/audio.c` backend and
  links `src/rt/audio_unavailable.c` instead.
- **Vulkan** — Apache-2.0 loader/header ecosystem; this repository does not
  redistribute a Vulkan SDK or loader binary.
- **shadcn/ui** — MIT notice for the dashboard primitives is preserved in
  `THIRD_PARTY_LICENSES/SHADCN_UI.txt`.
- **VFPU tables** — PPSSPP-origin lookup data with the exact source and checksums
  recorded in `assets/vfpu/PROVENANCE.json`; inclusion is a provenance decision,
  not a claim about PSP firmware ownership.

## Public-safe exclusions

The public source profile excludes the PGF parser/font payloads and PGD/amctrl
implementation/tooling pending qualified review. It also excludes the sal063-
derived ISO/VFS and SDL audio implementations until their separate public review
is complete. These exclusions are enforced by
`assets/public_source_profile.json`, not by a disclaimer. The unavailable seams
reject the capability and do not fabricate fonts, disc reads, playback, keys, or
success.

The HST-specific title manifest and private engineering/review documents are also
outside the active public source tree. Synthetic title manifests and generic schemas are
retained; no game executable, extracted asset, save, key, capture, oracle trace,
or generated retail translation unit is part of the public source boundary.

## Attribution contract

Preserve each file's SPDX header and any upstream notice when copying or
modifying source. The public provenance ledger assigns every included path an
explicit class, evidence reference, and content hash. A missing or unresolved
record is a publication failure. Generated files remain generated and must not be
hand-edited.

## Trademark and compatibility

Product and game names are used only to identify compatibility and research scope.
Nakagawa Recomp is not affiliated with or endorsed by Sony Interactive
Entertainment, Clap Hanz, PPSSPP, sal063, FFmpeg, SDL, Khronos, or any other
upstream author.

## Private inputs and history

Users must supply any lawful external inputs locally. Private inputs, keys, saves,
captures, extracted assets, generated output and private repository history must
not be committed, attached to issues, or included in a release. A clean current
tree is not historical clearance: the exact proposed history must pass the
reachable-object/content audit before publication.

The active public repository deliberately begins with the sanitized restoration
lineage. Former private/pre-sanitization development history is not ordinary
`main` ancestry and must not be reconnected to it.

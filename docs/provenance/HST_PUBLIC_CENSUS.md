# HST-related public-boundary census

This is a source-boundary record, not a gameplay or hardware acceptance claim. It
classifies the HST-facing paths that can be present in the development checkout and
states which of them may cross the public-source boundary.

| Surface | Public disposition | Provenance / boundary decision |
| --- | --- | --- |
| `assets/titles/hst-ucus98701.json` | Excluded | Title-specific identity, module addresses and private-route filesystem configuration. The public candidate retains only generic schema and synthetic title manifests. |
| `assets/titles/title_manifest.schema.json` | Included | Generic schema; it contains no title bytes, addresses, hashes, keys or route evidence. |
| `assets/titles/synthetic.json`, `assets/titles/pspdev-phase5.json` | Included | Source-owned fixtures with synthetic addresses and build paths. They do not describe retail content. |
| `hst.ps1`, `hst_manager.ps1` | Included as tooling | Generic orchestration and fail-closed input validation. A user-supplied title manifest and local inputs are required; the scripts do not carry those inputs. |
| `tools/hst_*.py`, `tools/hst_*.ps1` | Included as tooling | Source-owned parsers, doctor checks and synthetic regressions. They may name input *slots*, but do not contain retail bytes, captures, saves, keys or derived output. |
| `tools/title_*` and code-generation helpers | Included as generic tooling | The public contract is deterministic generation from a user-provided ELF/PRX. Generated retail translation units and private executable inputs are never tracked or exported. |
| `Makefile`, `copy_build_assets.ps1`, runtime build scripts | Included as build contract | Public-safe selection chooses unavailable ISO/audio/PGF/PGD boundaries when the optional implementations are absent. |
| `src/rt/` generic runtime and selftests | Included where enumerated | Runtime behavior is not evidence that HST is playable or that a private route is reproducible. No title image, decrypted module, save, trace, capture or extracted asset is part of this census. |
| `build/`, `logs/`, `memstick/`, `oracle/`, `keys/`, `place_game_here/`, extracted data and generated chunks | Never-scope | Ignored/private/generated material. The publication audit rejects these paths and the history audit scans reachable blob contents for their accidental reappearance. |

## Census rules

1. A title-specific path is excluded even when it contains no obvious binary or key.
   Names, addresses, route names and filesystem layouts can be derived from private
   inputs and are not required by a generic source release.
2. A public tool may accept a private input locally, but the input, output, capture,
   save and oracle are not provenance for the public tree and must not be checked in.
3. HST-specific runtime or visual acceptance remains a separate private evidence lane.
   This document deliberately makes no claim about it.
4. Any new HST path must be classified here, added to the machine-readable profile,
   and given an explicit provenance-ledger record before it can be considered for a
   candidate export. Unknown paths fail closed.

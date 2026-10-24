# PROVENANCE.md — `src/rt/minimp3/`

Single-header MPEG-1/2/2.5 audio decoder (Layers I, II and III) for the
Nakagawa recomp runtime. It decodes the frames behind the PSP `sceMp3`
library. The header is imported unmodified; the project's narrow wrapper is
`src/rt/mp3_bridge.c` and `src/rt/mp3_bridge.h`, which are project-authored and
live outside this directory.

## Upstream source

- Project: minimp3
- Repository: `https://github.com/lieff/minimp3`
- Pin: commit `ea99364f61c14656440e8d77e9c233ccf3124633` (the `master` head when
  imported on 2026-10-24; committed 2026-07-27, "Don't enable ARMv6 features for
  ARMv6-m"). Blob SHAs below are `git rev-parse <commit>:<path>` against that
  commit's tree.
- Licence: CC0 1.0 Universal public-domain dedication (`LICENSE`, imported
  byte-identical from the same commit). The dedication notice at the top of
  `minimp3.h` is kept unchanged.
- Owner decision 2026-10-24: officially public-domain code may be vendored
  without a per-import request, with the normal pin, manifest, credit and
  admission.
- Import method: `git clone` of the upstream repository, files copied from the
  pinned tree, line endings normalised to LF by this repository's
  `.gitattributes` (upstream stores LF; the blob hashes match).

## File manifest

| File | Kind | Upstream blob SHA |
| --- | --- | --- |
| `LICENSE` | imported byte-identical | `2c4afabdb632b5d68e60c9fe9fe0b0819676a729` |
| `minimp3.h` | imported byte-identical | `9fbd4d2cc50c06cc3e63dcafdd506dc6512b2103` |

There is no LOCAL DELTA. Upstream's `minimp3_ex.h` (file and seek helpers),
tests, fuzzers, player and vectors are not imported: the runtime feeds frames
from guest memory and needs only the frame decoder.

## Build configuration

`src/rt/mp3_bridge.c` is the only translation unit that defines
`MINIMP3_IMPLEMENTATION`. It leaves the upstream defaults in place (integer
`int16_t` output; SSE2/NEON paths where the compiler enables them), so the
decoder is compiled exactly as upstream ships it.

## Verification

```sh
git hash-object src/rt/minimp3/minimp3.h   # 9fbd4d2cc50c06cc3e63dcafdd506dc6512b2103
git hash-object src/rt/minimp3/LICENSE     # 2c4afabdb632b5d68e60c9fe9fe0b0819676a729
```

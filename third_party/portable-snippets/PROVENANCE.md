# PROVENANCE.md — `third_party/portable-snippets/`

The overflow-checked arithmetic header from Portable Snippets, vendored for the
checked size arithmetic in `src/core/nk_xb.c` (`checked_mul_size`, the XB
filesystem table size) and `src/core/nk_iso.c` (`nk_iso_checked_add_u64`, the
ELF program and section header table ends). Only `safe-math` is imported; the
other snippets have no use in the project yet.

## Upstream source

- Project: Portable Snippets
- Repository: `https://github.com/nemequ/portable-snippets`
- Pin: commit `84abba93ff3d52c87e08ba81de1cc6615a42b72e` (2021-03-15), the
  head of the default branch `master` at the time of import. Upstream has no
  release tags.
- License: `CC0-1.0`. `safe-math.h` carries its own CC0 1.0 waiver in its
  header comment; `COPYING.md` explains that each file's preamble governs it
  and reproduces the CC0 1.0 Universal text. The project adopted this import
  under the owner's 2026-10-24 rule that officially public-domain code (CC0,
  Unlicense, an explicit public-domain dedication) may be vendored with the
  normal bookkeeping.
- Import method: both files were downloaded from
  `raw.githubusercontent.com` at the pinned commit. Their Git blob IDs match
  the files in that commit, and the SHA-256 values below were calculated from
  the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `safe-math/safe-math.h` | imported byte-identical | `d67a985345500f9111d315c6c63380c4a06ff821` | `f0ec77d1f12954a03301bcf39cd58a4b266639cf403e469df50f49a107f55d0b` |
| `COPYING.md` | imported byte-identical | `77b4b2193e04b7734a12e3dc8b055cdc1f46c9b8` | `ff5ac505f6fd6dd3c841f210db469a5dd2fc00a81559b0487c15357f6e2bd331` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Local deltas

None. The header declares `__int128` helper types, which GCC reports under
`-Wpedantic`, so the project's call sites wrap the include in
`#pragma GCC diagnostic ignored "-Wpedantic"` instead of editing the header.
`tests/native/test_safe_math.c` (run by `native-core-tests`) includes it the
same way under `-Wpedantic -Werror` and checks the size and 64-bit edges those
call sites rely on.

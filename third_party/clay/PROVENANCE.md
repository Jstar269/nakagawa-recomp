# PROVENANCE.md — `third_party/clay/`

Single-header Clay layout library vendored for the native player's UI. The
launcher, the in-game overlay and their tests lay out through it:
`src/ui/nk_ui_clay.c` is the one source that compiles the implementation, and
`tests/native/test_clay_layout.c` checks the header on its own.

## Upstream source

- Project: Clay
- Repository: `https://github.com/nicbarker/clay`
- Pin: commit `e6cc36941ab2af5d81107617039d6f527a1c660b` (2026-05-20), the
  head of the default branch `main` when it was imported on 2026-10-29. This is
  a branch-head pin by owner decision: no release has been tagged since `v0.14`
  (commit `b25a31c1a152915cd7dd6796e6592273e5a10aac`, the previous pin), and
  `main` is 99 commits ahead of it.
- License: `Zlib`. Upstream's `LICENSE.md` calls it the "zlib/libpng license";
  the complete upstream license text is kept beside the header.
- Import method: `clay.h` and `LICENSE.md` were downloaded from the pinned
  commit. Their Git blob IDs match the corresponding files in that commit, and
  the SHA-256 values below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream blob SHA at the pin | SHA-256 |
| --- | --- | --- | --- |
| `clay.h` | imported + LOCAL DELTA | `7d9cf4cc7a918d1bdd766867c180851c949a3310` | `d9e5e2b1bc2662f6a85c14a3d3d64253057d38a7241c1d870a936aca80483f83` |
| `LICENSE.md` | imported byte-identical | `2968c61bcdc6ecbef470e02edd45b47aba1d8c44` | `5dd30f9f52093076b7574458f5efb8242fe6cbbd4669d4a2914e7616810d0df8` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

No generated Clay file is included. The upstream `clay.h` bytes have SHA-256
`54aef6d5f96b14e296bebb6db4a755543e98fd7e6c188411afb2f22a4b4f7728`;
applying the four changes below to them gives the SHA-256 in the table.

## Local deltas

Each change is marked `LOCAL DELTA` in the file, and
`tests/native/test_clay_layout.c` fails on the upstream bytes for each of them.

- `clay.h`, `Clay__Array_Allocate_Arena`: `capacity * itemSize` is computed as
  `(size_t)capacity * itemSize`, so the product is formed in `size_t` instead of
  32 bits before the arena bounds check (CodeQL "multiplication result
  converted to larger type"). Every in-range result is unchanged. Upstream at
  the pin still multiplies in 32 bits, so a request of 65536 items of 65536
  bytes wraps to 0 and is granted; the selftest checks that it is refused.
- `clay.h`, `Clay__CalculateFinalLayout`: a wrapped text element's minimum height
  is raised to its wrapped height, and the pass that carries wrapped heights up
  the tree also carries the minimum heights, with the rule an element's close
  uses (a sum along a column, the tallest child across a row, nothing for a
  vertical clip container). Upstream at the pin still takes the minimum before
  wrapping, as one line, so compressing an overfull parent squeezes wrapped text
  back toward one line and the next element paints over its later lines; the
  selftest checks the overfull case.
- `clay.h`, `Clay__AddHashMapItem`: the element-id map is full only when a new
  slot is needed and its free list is empty. Upstream at the pin checks the
  slot array's length first, before a repeated id or a freed slot can be used,
  and that array never shrinks: after one layout passes the element limit,
  every later layout fails with `HASH_MAP_CAPACITY_EXCEEDED`, though pruning
  has freed the slots of the elements that are gone. The selftest lays out past
  the limit, then checks that the next layout succeeds without an error.
- `clay.h`, `Clay_UpdateScrollContainers`: a released drag keeps momentum only
  when its frames passed time. Momentum is the drag's distance over its time,
  and upstream at the pin divides by that time even when it is zero (a host
  whose clock stood still across the drag's frames): the infinite momentum pins
  the container at one end until the next scroll. The selftest drags with a
  `deltaTime` of 0 and checks the container stays where the drag left it.

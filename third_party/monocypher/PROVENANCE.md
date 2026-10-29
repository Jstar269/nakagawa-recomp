# PROVENANCE.md — `third_party/monocypher/`

The Ed25519 signature check and BLAKE2b hash from Monocypher, vendored for the player's signed update download
([`docs/RELEASE_SIGNING.md`](../../docs/RELEASE_SIGNING.md); [`docs/ONLINE_FEATURES.md`](../../docs/ONLINE_FEATURES.md#implementation-order),
item 3). The project wrappers are `nk_ed25519_verify` and `nk_minisign_verify` in `src/core/nk_sig.c`; the player's
update check (`src/player/update_check.c`) calls them to check a release's minisign signature, and the native
selftest `tests/native/test_nk_sig.c` (`ed25519-verify-selftest`, also run by `native-core-tests`) tests them. The
vendored files contain no network code and no key.

## Upstream source

- Project: Monocypher
- Repository: `https://github.com/LoupVaillant/Monocypher`
- Pin: release tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` (published 2026-06-15, the newest
  release at the time of import).
- License: dual `BSD-2-Clause OR CC0-1.0` (each file's SPDX line; `LICENCE.md` lets the user choose). The project
  elects **CC0-1.0**, the public-domain dedication, under the maintainer's 2026-10-24 rule for officially
  public-domain code. The complete upstream `LICENCE.md`, with both licences and the author list, is kept beside the
  sources unchanged.
- Import method: the four source files and `LICENCE.md` were downloaded from the tag's release tarball and again
  from `raw.githubusercontent.com` at the pinned commit; both copies are identical. Their Git blob IDs match the
  pinned commit's tree, and the SHA-256 values below were calculated from the local bytes.
- Only what Ed25519 verification needs is vendored: the core library (`monocypher.c`, `.h`) and the optional
  Ed25519 file (`src/optional/monocypher-ed25519.c`, `.h`), which supplies the SHA-512 that RFC 8032 Ed25519
  requires (the core library's EdDSA uses BLAKE2b). The optional pair is flattened into this directory; its
  `#include "monocypher.h"` resolves here without an edit.

## File manifest

| File | Upstream path | Kind | Upstream 4.0.3 blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `monocypher.c` | `src/monocypher.c` | imported byte-identical | `c7c5450e84e4dffda44514d9f918162f1830de56` | `f1f838cdd483bdebe0df0ff5c5ed60535e496f769c6a2f933ac4c0b114207123` |
| `monocypher.h` | `src/monocypher.h` | imported byte-identical | `cf635e88e4f5851e1f58a9e2b3e85b8029a73861` | `fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3` |
| `monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | imported byte-identical | `4fdd07641e1c4d54a348e95d0ab05c28f84654a6` | `ce0d2f8e32ca8f66398ba5b3456cc74327c3eff14e7b950ce7d57be9025cc453` |
| `monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | imported byte-identical | `d7aa004123e1d38b891fdabd4171fbf9957242ae` | `3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce` |
| `LICENCE.md` | `LICENCE.md` | imported byte-identical | `9d69ad72ec113a743264d34dd86a5c04d0c42d35` | `a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0` |
| `PROVENANCE.md` | n/a | project-authored manifest | n/a | n/a |

## Local deltas

None. The files build warning-free with `-std=c99 -Wall -Wextra -Wpedantic` under MinGW-w64 GCC and Linux GCC, so
no compile flag or source edit is needed.

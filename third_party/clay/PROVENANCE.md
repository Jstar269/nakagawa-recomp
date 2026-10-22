# PROVENANCE.md — `third_party/clay/`

Single-header Clay layout library vendored for the native player's future C UI.
The current player has no Clay UI call sites; the source-owned native selftest
is the only project build target that compiles the header.

## Upstream source

- Project: Clay
- Repository: `https://github.com/nicbarker/clay`
- Pin: tag `v0.14`, commit
  `b25a31c1a152915cd7dd6796e6592273e5a10aac`. This is the newest release tag
  listed by upstream at the time of import.
- License: `Zlib`. Upstream's `LICENSE.md` calls it the "zlib/libpng license";
  the complete upstream license text is kept beside the header.
- Import method: `clay.h` and `LICENSE.md` were downloaded from the `v0.14`
  tag. Their Git blob IDs match the corresponding files in the pinned upstream
  commit, and the SHA-256 values below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream v0.14 blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `clay.h` | imported byte-identical | `8ee7f5044d73ce8b7444fa4a7a6355730511dd7f` | `c97241cc423af3fa11267978adce9cbb46274a2ad0709a5d4b2b1092dc27599d` |
| `LICENSE.md` | imported byte-identical | `2968c61bcdc6ecbef470e02edd45b47aba1d8c44` | `5dd30f9f52093076b7574458f5efb8242fe6cbbd4669d4a2914e7616810d0df8` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

The release tag resolves directly to the pinned commit. No branch-head source,
local patch, or generated Clay file is included.

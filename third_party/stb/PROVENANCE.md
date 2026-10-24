# PROVENANCE.md — `third_party/stb/`

Sean Barrett's single-header PNG decoder, vendored for the native player's next UI: `src/ui/nk_ui_image.c` builds
it for PNG only, from memory, with no file access, to decode the project's embedded prompt glyphs and, at run time,
the `ICON0.PNG` and `PIC1.PNG` the player reads from the user's own disc image. Those disc images are never written
to disk or stored in this repository.

## Upstream source

- Project: stb (`stb_image`, v2.30)
- Repository: `https://github.com/nothings/stb`
- Pin: commit `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` (`master` on 2026-10-24). `stb_image.h` last changed in
  commit `013ac3beddff3dbffafd5177e7972067cd2b5083` (2024-05-31, v2.30), so the file is the same at both.
- License: upstream offers a choice of MIT or public domain (Unlicense wording), in `LICENSE` and at the end of
  `stb_image.h`. The project uses it under the public-domain alternative (`Unlicense`). Both texts are kept unchanged.
- Import method: both files were downloaded from the pinned commit. Their Git blob IDs match the pinned commit's
  files, and the SHA-256 values below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `stb_image.h` | imported byte-identical | `9eedabedc45b3e6fd88fae6f14a160b4d53272ec` | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` |
| `LICENSE` | imported byte-identical | `a77ae91f3ec0abb0028c9d9bd8d34d880b454d38` | `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Build configuration

`src/ui/nk_ui_image.c` includes the header with `STBI_ONLY_PNG`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR`,
`STBI_NO_FAILURE_STRINGS`, `STBI_NO_THREAD_LOCALS`, `STBI_MAX_DIMENSIONS 8192` and `STB_IMAGE_STATIC`. Before stb
reads anything past the header, the caller's byte and dimension limits are applied, so an oversized image is refused
without being decoded.

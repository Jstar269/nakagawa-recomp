# PROVENANCE.md — `third_party/xbrz/`

xBRZ is Zenju's pixel-art scaling algorithm. Nakagawa runs it in the Vulkan GE renderer's
texture scaling (`src/rt/gpu_sdl3vk/tex_scale.c`, Ace Mode only): a guest texture is decoded,
scaled 2x to 6x on a worker thread, and the copy is drawn in its place. The scaler is a C
translation of the upstream C++ release, so it builds with the runtime's C toolchain and needs
no C++ runtime.

## Upstream source

- Project: xBRZ
- Home: `https://sourceforge.net/projects/xbrz/`
- Pin: release 1.9 (`Changelog.txt`: "xBRZ 1.9 [2026-01-24]"), the newest release when imported on
  2026-11-04. Downloaded as `xBRZ_1.9.zip` from
  `https://sourceforge.net/projects/xbrz/files/xBRZ/xBRZ_1.9.zip/download`; the zip's SHA-256 is
  `b2dff73b3abd24a18a7cde78d5ff5ed8f0922296dce6ed734dce2264cd0a0fc9`. Upstream publishes release
  archives, not a Git history, so the blob IDs below are `git hash-object --no-filters` of the
  files in that zip.
- Licence: GNU General Public License version 3 (`License.txt`). Each upstream source file's
  header adds the author's linking exception for MAME, FreeFileSync, Snes9x and ePSXe. The header
  is kept unchanged in both translated files, followed by a note that the file was modified.
- Imported under `AGENTS.md` section 4's import rules. The release is the original source; the
  copies other projects carry (for example PPSSPP's `ext/xbrz`) were not used.

## File manifest

| File | Kind | Upstream file | Upstream blob | SHA-256 in this tree |
| --- | --- | --- | --- | --- |
| `License.txt` | LOCAL DELTA: CRLF line endings normalised to LF | `License.txt` | `ea09e3734b6a49a0c0c4f123d0451db78522408c` | `c61f12da7cdad526bdcbed47a4c0a603e60dbbfdaf8b66933cd088e9132c303f` |
| `xbrz.c` | imported + LOCAL DELTA: translated from C++ to C | `xbrz.cpp`, `xbrz_config.h`, `xbrz_tools.h` | `a027586a3043c723fef8c4ae54eb876953ec18df` (`xbrz.cpp`) | `2d9c4c06d3a10f85b8dcfa888ebae09f88f6d27c4b2ed9520e51944e54806cc3` |
| `xbrz.h` | imported + LOCAL DELTA: translated from C++ to C | `xbrz.h`, `xbrz_config.h` | `f92b65cf1daf314db2af0025f46000bdc0a0bacc` (`xbrz.h`) | `c469a502146cad9dcbf4384073da988367c5757631bf5b04d42a3d2b698d095d` |

The other upstream inputs to the translation: `xbrz_config.h` (blob
`238045e625842dca4cce01d36681407782ac07f8`) and `xbrz_tools.h` (blob
`0ce91874dc850383b1e0f062b38c5d9c508cea73`). `Changelog.txt` is not imported.

Project-authored files in this directory (GPL-3.0-or-later):

| Path | Purpose |
| --- | --- |
| `PROVENANCE.md` | this manifest |
| `xbrz_golden.cpp` | builds against the unmodified upstream release and prints the goldens the C port is checked against |

## Local deltas

The translation keeps upstream's algorithm and constants and changes only what C needs:

- **One colour format.** Only `ColorFormat::argbUnbuffered` is kept: ARGB pixels, colour distance
  computed per call (upstream's `ARGB_UNBUFFERED` path), so the 64 MB distance table of the
  buffered paths is never built. Guest textures carry alpha, which rules out `rgb`; upstream
  describes `argbUnbuffered` as `argb` without the one-time table build, at a slightly slower
  scale. `ColorFormat::rgb`, `argb` and `bilinearScale`/`nearestNeighborScale` are not
  translated.
- **Templates to tables.** Upstream's `Scaler2x` to `Scaler6x` classes and the `RotationDegree`
  template become one function per factor that takes the rotation at run time
  (`k_rot_getter`); `blendPixel` and `preProcessCorners` are plain functions. The pixel each
  rotation reads is the same as upstream's `get_*` specialisations.
- **The out-of-bounds reader.** Upstream's default `OobReaderTransparent` is kept: pixels outside
  the image read as transparent. The runtime pads the image with two texels of its own (repeated
  or clamped, by the texture's wrap mode) and crops the result, so the scaler never sees the
  border (`tex_scale.c`).
- **Floating point.** `#pragma GCC optimize("fp-contract=off")` keeps the compiler from fusing
  the colour-distance arithmetic into FMA, which would change rounding against upstream.
- **API.** `nk_xbrz_scale(factor, src, trg, w, h, cfg, yFirst, yLast)` and `NkXbrzCfg` with
  upstream's defaults (`nk_xbrz_default_cfg`), in place of `xbrz::scale` and `xbrz::ScalerCfg`.
  A bad factor returns 0 instead of asserting.

## Verification

`xbrz_golden.cpp` is compiled against the unmodified release and prints a 64-bit FNV-1a hash of
every 2x to 6x scale of two synthetic patterns (40x28 and 23x17: diagonals, blocks, lone
texels, alpha), plus every pixel of one 2x corner. The selftest
`src/rt/gpu_sdl3vk/tex_scale_selftest.c` (`make tex-scale-selftest`) checks the C port against
those values. The port matched at `-O0`, `-O2` and `-O2 -march=native`.

```sh
g++ -std=c++23 -O2 -ffp-contract=off -I<xBRZ_1.9> third_party/xbrz/xbrz_golden.cpp \
    <xBRZ_1.9>/xbrz.cpp -o xbrz_golden
./xbrz_golden
git hash-object --no-filters <xBRZ_1.9>/xbrz.cpp   # a027586a3043c723fef8c4ae54eb876953ec18df
sha256sum third_party/xbrz/License.txt            # c61f12da7cdad526bdcbed47a4c0a603e60dbbfdaf8b66933cd088e9132c303f
```

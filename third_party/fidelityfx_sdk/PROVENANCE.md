# PROVENANCE.md — `third_party/fidelityfx_sdk/`

AMD FidelityFX Super Resolution 1 (FSR 1) as the AMD FidelityFX SDK carries it: EASU, the
edge-adaptive spatial upscaler, and RCAS, the robust contrast-adaptive sharpener, with the SDK's
`ffx_core` portability headers. The project's compute shaders
`src/rt/gpu_sdl3vk/shaders/fsr1_easu.comp` and `fsr1_rcas.comp` include them. They are compiled into
the native player's opt-in presentation upscaler (`SR_PRESENT_UPSCALE=fsr1`). No C file includes
them: the SDK's CPU layer is C++, so EASU computes its constants in the shader
(`ffxFsrPopulateEasuConstants`), and `src/rt/gpu_sdl3vk/fsr1_pass.c` sets RCAS's single constant
itself.

The folder mirrors the SDK's `Kits/FidelityFX/` layout, so the headers' own relative includes work
unchanged, and further SDK components can be added beside these files under this one manifest.

## Upstream source

- Project: AMD FidelityFX SDK
- Repository: `https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`
- Pin: commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` (2026-10-28, "AMD FSR SDK 2.4.0"), the
  latest release at the time of import. This copy of FSR 1 replaces the archived standalone
  FidelityFX-FSR v1.0.2 (2021) the presenter used before.
- GLSL layer: SDK 2.4.0's `ffx_core.h` still includes `ffx_core_glsl.h` for a GLSL build, but the
  SDK has not shipped that file since 2.0.0 (2025-08-20). It comes from commit
  `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` (2025-05-08, tag `v1.1.4`, "AMD FidelityFX SDK 1.1.4"),
  the last release that ships it. Its blob is unchanged from SDK 1.1 (2024-07-04), so the file's
  own notice says 2024. Nothing from 2026 replaces it, and all the algorithmic code (EASU, RCAS
  and the `ffx_core` helpers they call) is from 2.4.0. The SDK's GLSL path is retired, so this file
  is due to go once the project compiles the SDK's HLSL to SPIR-V (the FSR 3 lane's pinned DXC
  toolchain).
- License: MIT. The SDK's `docs/license.md` at the 2.4.0 pin applies a binary-only licence to the
  repository by default and lists each file it releases under MIT. Every 2.4.0 file here is on
  that MIT list, checked path by path. SDK v1.1.4 is MIT as a whole (`LICENSE.txt` at its root). Every
  header carries the MIT notice (Copyright (C) 2026 Advanced Micro Devices, Inc. for the 2.4.0
  files, 2024 for `ffx_core_glsl.h`). `LICENSE.txt` here is SDK v1.1.4's root `LICENSE.txt`, the
  same MIT text.
- Import method: `git show <pin>:<path>` from a clone of the repository, so the content is the Git
  blob. Upstream stores every file here with LF line endings, so each is imported byte-identical:
  `git hash-object` of the file in this tree equals the upstream blob ID below
  (`git rev-parse <pin>:<path>`). The SHA-256 values are of the same bytes.

## File manifest

| File | Kind | Upstream path | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `api/internal/gpu/ffx_common_types.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_common_types.h` | `076e2828973f4300962a8124bac22f48a071213c` | `30b4f299e48eddaa64d9ad56c259fc13688fa5bc5735475c84d9ae10fd94907c` |
| `api/internal/gpu/ffx_core.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core.h` | `fdeecf6cd9c00ebf589c34a26321206199c8fe11` | `db3268fc71b7663ca4b51504998602dde9e91262385efbf81acdc9a790133a4d` |
| `api/internal/gpu/ffx_core_gpu_common.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_gpu_common.h` | `4ef2dbd23ade02da7cf95da1d90de3c22b646ae0` | `2df061c94b04bb4dedb6931a9e8241c1aa94eae826c506a75851a7f72ea4f4e8` |
| `api/internal/gpu/ffx_core_gpu_common_half.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_gpu_common_half.h` | `3270ab12048df507fac0bcbde4e1859ecc028747` | `1f7368a8cd327c87c6b8031cf8c2aa32a6c46bb39d8fadf151212dcfd5d61900` |
| `api/internal/gpu/ffx_core_portability.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_portability.h` | `b488f5c97a72e61c5c48b41a09274681a3eea702` | `217281792d09b88ad122055a6361a9303ccbce22238172fa6b30df7978d38968` |
| `api/internal/gpu/ffx_core_glsl.h` | imported byte-identical (SDK v1.1.4, commit `c6efa6bf`) | `sdk/include/FidelityFX/gpu/ffx_core_glsl.h` | `c8dccacdbc44460160d3b9fe3ce569ea2b2615e8` | `01afe5b65549b2c0951b494ca4bf570ee2aa89a64344e4c7d98ef86e6613ab42` |
| `upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h` | `2ae12de1074188695a51f55123bede2c301fdd8b` | `328ee880b26f400f7d8e59ffcdf55b83ae71d6de759e72f106a3c8cfc0d4cc8e` |
| `LICENSE.txt` | imported byte-identical (SDK v1.1.4, commit `c6efa6bf`) | `LICENSE.txt` | `3712e34adea415aa00dab08625ce7224b27f1706` | `82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a | n/a |

## Local deltas

None: every file is imported byte-identical. The 2.4.0 headers needed no helper that the SDK v1.1.4
GLSL layer lacks: the shaders compile with them as imported. Upstream ends some lines in spaces and indents one line of
`ffx_core_glsl.h` with a tab, so `.gitattributes` exempts the imported files from the whitespace
checks.

## How FSR 1 changed from v1.0.2

The presenter used the standalone FidelityFX-FSR v1.0.2 headers before this import. Compared
function by function, with the move from the `A_` portability macros to `ffx_core` names set aside:

- EASU is unchanged. Its constant packing is the same.
- RCAS changed in one place, its limiter, in all three variants (32-bit, 16-bit, packed 16-bit).
  v1.0.2 computes `hitMin = min(mn4, e) / (4 * mx4)` and `hitMax = (1 - max(mx4, e)) / (4 * mn4 - 4)`.
  2.4.0 drops the centre pixel from both and scales `hitMin` by
  `saturate(eL / min(bL, dL, fL, hL))`, the centre's luma over the darkest ring luma.
  `FSR_RCAS_LIMIT`, the denoise term and the constant packing are unchanged.

## Updating

The embedded SPIR-V (`src/rt/gpu_sdl3vk/fsr1_easu_comp.inc`, `fsr1_rcas_comp.inc`) is compiled
from the two `.comp` files, which include these headers. `shader_manifest.json` hashes the `.comp`
sources only, so `tools/shader_embed.py verify` does not notice a header change: after any change
to a header here, regenerate the embeddings with `tools/shader_embed.py`.

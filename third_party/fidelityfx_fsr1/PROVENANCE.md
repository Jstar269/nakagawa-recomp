# PROVENANCE.md — `third_party/fidelityfx_fsr1/`

AMD FidelityFX Super Resolution 1 (FSR 1): EASU, the edge-adaptive spatial upscaler, and RCAS, the
robust contrast-adaptive sharpener. The two headers are included by the project's compute shaders
`src/rt/gpu_sdl3vk/shaders/fsr1_easu.comp` and `fsr1_rcas.comp`, and by
`src/rt/gpu_sdl3vk/fsr1_pass.c`, which uses only their CPU constant setup (`FsrEasuCon`, `FsrRcasCon`).
They are compiled into the native player as the opt-in presentation upscaler (`SR_PRESENT_UPSCALE=fsr1`).

## Upstream source

- Project: AMD FidelityFX Super Resolution 1 (standalone repository)
- Repository: `https://github.com/GPUOpen-Effects/FidelityFX-FSR`
- Pin: commit `a21ffb8f6c13233ba336352bdff293894c706575` (2021-11-15, "FidelityFX FSR v1.0.2"), the
  head of the default branch at the time of import. `ffx_fsr1.h` names its version `v1.20210629`.
  The FidelityFX SDK also carries FSR 1, but as a port to the SDK's own `ffx_core` headers with the
  same algorithm. This standalone pair is the original form and needs nothing else.
- License: MIT. Copyright (c) 2021 Advanced Micro Devices, Inc. The notice is kept at the top of both
  headers and in `LICENSE.txt`.
- Import method: `git show <pin>:<path>` from a clone of the repository at the pin, so the bytes are
  the Git blobs: LF line endings, unchanged. Upstream `license.txt` is stored as `LICENSE.txt`, with
  its bytes unchanged.

## File manifest

| File | Kind | Upstream path | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `ffx_a.h` | imported byte-identical | `ffx-fsr/ffx_a.h` | `d04bff55cbe5f0c77af7a39d6a3452f2744dc6f4` | `f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb` |
| `ffx_fsr1.h` | imported byte-identical | `ffx-fsr/ffx_fsr1.h` | `4e0b3d548553e61eedd384021409f24e3eb74de3` | `93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5` |
| `LICENSE.txt` | imported byte-identical (upstream `license.txt`) | `license.txt` | `324cba594d11f918191907676a99deed2fea51d0` | `db089274ce766da70f5b7d791029c3486f9f9e27c8c79c652689603d3192e802` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a | n/a |

## Local deltas

None. Upstream ends some lines of both headers in spaces. The repository's `.gitattributes` exempts
those two files from the trailing-space check, so the bytes stay identical to upstream.

The embedded SPIR-V (`src/rt/gpu_sdl3vk/fsr1_easu_comp.inc`, `fsr1_rcas_comp.inc`) is compiled from
the two `.comp` files, which include these headers. `shader_manifest.json` hashes the `.comp` sources
only, so `tools/shader_embed.py verify` does not notice a header change: after any change to a
header here, regenerate the embeddings with `tools/shader_embed.py`.

# PROVENANCE.md — `third_party/fidelityfx_sdk/`

AMD FidelityFX Super Resolution 3.1.6 (FSR 3), the temporal upscaler, as the AMD FidelityFX SDK
carries it: the eight compute passes' HLSL, the upscaler headers they include, single-pass
downsampling (SPD), RCAS from FSR 1, and the SDK's `ffx_core` portability headers.
`tools/shader_embed.py` compiles each pass to SPIR-V with a pinned DirectXShaderCompiler and
embeds it as `src/rt/gpu_sdl3vk/fsr3_*_comp.inc`; `fsr3_shader_manifest.json` records the
compiler, its options and the SHA-256 of every file each pass includes. No C file includes these
headers: the SDK's CPU layer is C++, and the project's C host (`src/rt/fsr3_host.c`,
`src/rt/gpu_sdl3vk/fsr3_pass.c`) is a translation of it with its own notices. The passes are built
only into the FSR 3 selftests so far; the player does not run them.

The folder mirrors the SDK's `Kits/FidelityFX/` layout, so the headers' own relative includes work
unchanged, and further SDK components can be added beside these files under this one manifest.

## Upstream source

- Project: AMD FidelityFX SDK
- Repository: `https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`
- Pin: commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` (2026-10-28, "AMD FSR SDK 2.4.0"), the
  latest release at the time of import. Its upscaler is FSR 3.1.6.
- License: MIT. The SDK's `docs/license.md` at the pin applies a binary-only licence to the
  repository by default and lists each file it releases under MIT, under its full
  `Kits\FidelityFX\...` path. Every file here is on that MIT list, checked path by path against the
  pin; `Kits/FidelityFX/docs/license.md` (blob `aef7adddbcf121c1f7f1248fe97de0f7f7a3d719`) lists
  the same files. The MIT listing, not the binary-only default above it, is the licence of each
  file here. Every header and shader carries the MIT notice (Copyright (C) 2026 Advanced Micro
  Devices, Inc.). `LICENSE-AMD.md` is the root `docs/license.md` at the pin, imported
  byte-identical, so the listing travels with the files.
- Import method: `git show <pin>:<path>` from a clone of the repository, so the content is the Git
  blob. Upstream stores every file here with LF line endings and no CR byte, so each file is
  imported byte-identical except the one LOCAL DELTA below: `git hash-object` of the file in this
  tree equals the upstream blob ID (`git rev-parse <pin>:<path>`). The SHA-256 values are of the
  same bytes.
- Not imported: every `.cpp`, the SDK's `backend/` and `api/internal/*.cpp`, `ffx_core_cpu.h`, the
  upscaler's debug-view and automatic-reactive passes, the DirectX 12 trees, and `signedbin/`.

## File manifest

| File | Kind | Upstream path | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `api/internal/gpu/ffx_common_types.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_common_types.h` | `076e2828973f4300962a8124bac22f48a071213c` | `30b4f299e48eddaa64d9ad56c259fc13688fa5bc5735475c84d9ae10fd94907c` |
| `api/internal/gpu/ffx_core.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core.h` | `fdeecf6cd9c00ebf589c34a26321206199c8fe11` | `db3268fc71b7663ca4b51504998602dde9e91262385efbf81acdc9a790133a4d` |
| `api/internal/gpu/ffx_core_gpu_common.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_gpu_common.h` | `4ef2dbd23ade02da7cf95da1d90de3c22b646ae0` | `2df061c94b04bb4dedb6931a9e8241c1aa94eae826c506a75851a7f72ea4f4e8` |
| `api/internal/gpu/ffx_core_gpu_common_half.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_gpu_common_half.h` | `3270ab12048df507fac0bcbde4e1859ecc028747` | `1f7368a8cd327c87c6b8031cf8c2aa32a6c46bb39d8fadf151212dcfd5d61900` |
| `api/internal/gpu/ffx_core_hlsl.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_hlsl.h` | `d617c5bbc8c6820fb91d9817db4824c60aca2668` | `c14a6bcb12f40284412b54c584bc86d424e5231fc579211008bcda547c3d5d05` |
| `api/internal/gpu/ffx_core_portability.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_portability.h` | `b488f5c97a72e61c5c48b41a09274681a3eea702` | `217281792d09b88ad122055a6361a9303ccbce22238172fa6b30df7978d38968` |
| `upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h` | `2ae12de1074188695a51f55123bede2c301fdd8b` | `328ee880b26f400f7d8e59ffcdf55b83ae71d6de759e72f106a3c8cfc0d4cc8e` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h` | `6789968c9a4ed559fdaba7525197be9508f62f73` | `d3d47fbf9c83bddbeb6f0d0f05f648b739a7edfaee287ea38fb18bf53ed478a3` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.h` | imported + LOCAL DELTA (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.h` | `6b5c5854735d2eeba70d60813be4e7aa77766ab4` | `c60817af0cd8f9ca28f0730be4cb2b14703797a116035f31a9b1965f2d280f8e` (this tree's bytes) |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_common.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_common.h` | `7fb6b93a7a8c178984534d9a533589c2619d460e` | `b4ee5b5dbf10df0e81d704cfff92507231fb3e3b93f06b9bfa8490f316e7bb66` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h` | `cef823dcf9dd8a27220ae1ada51e09f6862e072b` | `ea921c3af883fa5ef20d214749b269d793668ea916d97a749dd86c3320ad1d46` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h` | `87cf23e39c4490370969bf3e1f3e3d8cc72365e5` | `3f3362c5115688ad5a868c497c4ce45d0e0019cac51dac277e2f918dac76c242` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h` | `be74f0224df2677527a77112ee3efeab60491284` | `12e14745f8da1df56546873726c6f5ea10e26a944ebaff982fa10f18084280bb` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h` | `a68294f2906e8783559e5e72af1bd125324e4b47` | `1346c7dd03c39430bcac4e482fb4a33b3e45af76ecfb214cd61b833b036c59ee` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h` | `60b7014a9fe208b10bff3858a24c5bde5f46de26` | `7942c22fdaafaaf7648c20877104160e76d3fa2ab810a7f763e906bf7e3c36ba` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h` | `ea59015480bcc74577dfb2ae9b71482828232cad` | `2d6f878fd59444d767ff02214256ed93e8fd3a5b4b423bd1a90c9821b757b327` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h` | `913b9eabbc5f4e5b7ee2661815a63a0f999531bf` | `4190a25578ed28f56a83b1ed3c1cbd239cf73b622ff4819b8e8fc89e3eade433` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_sample.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_sample.h` | `8d5941254f4af455c0c8476cab661d33d9339740` | `4446848fedae924d28235839c5d2884c3daaa9f02d34f64912a96b670d285f03` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change.h` | `26d00b1989fe95bd19f90bfbf3f25653714acf3e` | `09097c77256ce5a0a43c286eb3d6e2a5568be7905d9b7374058135df18c2210e` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h` | `5bfaab20ab227e3b7a0e9c5668151e2640b3974c` | `69c7d21737ff16affe10fe27053b762debe535f304a451832dc20beadfe56301` |
| `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_upsample.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_upsample.h` | `3eeaef9904292aa0c1ede09c5addfe0c84ce604c` | `ccff9b22f7051b864d46d1d11a8efd0a0b8dd96d0cb2b94515e61df579deaf60` |
| `upscalers/fsr3/include/gpu/spd/ffx_spd.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/include/gpu/spd/ffx_spd.h` | `c1ed5371ed472b76d45def59960a8ef52e5565a6` | `4d3b02d58b215f7d0d3fcdc4cabfa8f2cd03a8efdde12f770c2142b1ccc81033` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_accumulate_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_accumulate_pass.hlsl` | `842a64af2282a698a53de38032e25f6fb1589b55` | `a9970dd929cda29a15acf1546a999db2ad2c49d8bf5a58a93797db09cbaf4fb1` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_luma_instability_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_luma_instability_pass.hlsl` | `2416f869f5c108e597d00a10bdae276ef4cca76c` | `5f2d50ea07bc46d9d27adc508ab013c6dc5137d4be67626ffaf4ef2593f5698a` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_luma_pyramid_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_luma_pyramid_pass.hlsl` | `555ea0baba4cbc547244c935effd5d063e6532f4` | `3d5be83c9d7ec601af52708788a8fc991c529a2e29fe4e340dc4b840a7154bc4` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_prepare_inputs_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_prepare_inputs_pass.hlsl` | `101d26cf6b605d93cd74934c08195e16c0391420` | `5312aed3cb16d0d639b33b9d36476d253e58342df2d7d2030ba5690a982f40d6` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_prepare_reactivity_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_prepare_reactivity_pass.hlsl` | `b8d1cbeb684d784fc7f87c0023df6c808c91869f` | `27b2f4ee4aca425d4e7b60925e80a4aae7057781044d08db4cacae3d183565d3` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_rcas_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_rcas_pass.hlsl` | `d6bc7f3da463a22ca92b0b991113fb2bd9f0844c` | `699fb6c19ceb57ebdffaedb2c50d9b1a6d4e9990ddd383056e70346aa484079d` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_shading_change_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_shading_change_pass.hlsl` | `c3ca70765f208ffe890cbb3befa706bbd9268a8f` | `30ae2b27d3a3fc520510be169e50a1a5ec937b505ee0a169fa82293e81c0e0eb` |
| `upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_shading_change_pyramid_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_shading_change_pyramid_pass.hlsl` | `802ea426d1d4acbd30fb7e09cb3073f50413ac2e` | `76b0e8fb9b9ead2516db1efb4239116c2ca0973c1c589d86f25a5adc6f8c17e4` |
| `LICENSE-AMD.md` | imported byte-identical (2.4.0) | `docs/license.md` | `abfeaf46c5aaea767cef4379d2890735dcc66207` | `410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a | n/a |

## Local deltas

One file, `upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.h` (Git blob
`49d6970164252de2a4eef2778b044099dd1f8c7f` in this tree). The passes are compiled with
`-fspv-use-unknown-image-format`, so a storage image declares no format in SPIR-V, and Vulkan
requires an image an atomic reaches to declare one (R32ui here; the `OpImageTexelPointer` rule of
the Vulkan specification's SPIR-V environment appendix). The delta adds `[[vk::image_format("r32ui")]]` to
the two `RWTexture2D<FfxUInt32>` declarations that atomics reach,
`rw_reconstructed_previous_nearest_depth` and `rw_spd_global_atomic`, and a three-line
modification notice after AMD's licence header. No other byte changes. `tools/test_fsr3_port.py`
reads the embedded SPIR-V and checks that the storage images declaring a format are exactly those
two, as R32ui, and that atomics reach both.

Upstream ends some lines in spaces, so `.gitattributes` exempts the imported files from the
whitespace checks; this manifest keeps the normal rules.

## Updating

After any change to a file here, regenerate the embeddings with the pinned compiler:
`python tools/shader_embed.py regenerate --shader-set fsr3 --dxc <DirectXShaderCompiler v1.9.2609>`.
`make fsr3-shader-verify` fails on any file whose SHA-256 no longer matches the manifest's include
closure, and `make fsr3-shader-repro-verify DXC=<dxc>` recompiles every pass and fails on any byte
difference.

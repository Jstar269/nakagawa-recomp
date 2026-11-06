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

FSR 3.1.6 frame generation sits beside it under `framegeneration/fsr3/`: the ten frame-interpolation
passes other than AMD's debug view, the seven optical-flow passes, and the headers they include.
They are compiled the same way as a second set, `fsr3fg_*_comp.inc` with
`fsr3fg_shader_manifest.json`, and the C host `src/rt/fsr3_fg_host.c` translates AMD's
`ffx_frameinterpolation.cpp` and `ffx_opticalflow.cpp`. They too run only in the selftests.

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
  imported byte-identical except the three LOCAL DELTA files below: `git hash-object` of the file
  in this tree equals the upstream blob ID (`git rev-parse <pin>:<path>`). The SHA-256 values are of
  the same bytes.
- Not imported: every `.cpp`, the SDK's `backend/` and `api/internal/*.cpp`, `ffx_core_cpu.h`, the
  upscaler's debug-view and automatic-reactive passes, frame interpolation's debug-view pass and its
  header `ffx_frameinterpolation_debug_view.h`, the frame-generation swapchain (`vk/` and the two
  `FrameInterpolationSwapchain*.hlsl` passes), the DirectX 12 trees, and `signedbin/`.
- `framegeneration/fsr3/include/gpu/spd/ffx_spd.h` is the same blob as the upscaler's copy; it is
  kept at its own path because the frame-generation headers include it relative to themselves.

## File manifest

| File | Kind | Upstream path | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `api/internal/gpu/ffx_common_types.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_common_types.h` | `076e2828973f4300962a8124bac22f48a071213c` | `30b4f299e48eddaa64d9ad56c259fc13688fa5bc5735475c84d9ae10fd94907c` |
| `api/internal/gpu/ffx_core.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core.h` | `fdeecf6cd9c00ebf589c34a26321206199c8fe11` | `db3268fc71b7663ca4b51504998602dde9e91262385efbf81acdc9a790133a4d` |
| `api/internal/gpu/ffx_core_gpu_common.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_gpu_common.h` | `4ef2dbd23ade02da7cf95da1d90de3c22b646ae0` | `2df061c94b04bb4dedb6931a9e8241c1aa94eae826c506a75851a7f72ea4f4e8` |
| `api/internal/gpu/ffx_core_gpu_common_half.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_gpu_common_half.h` | `3270ab12048df507fac0bcbde4e1859ecc028747` | `1f7368a8cd327c87c6b8031cf8c2aa32a6c46bb39d8fadf151212dcfd5d61900` |
| `api/internal/gpu/ffx_core_hlsl.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_hlsl.h` | `d617c5bbc8c6820fb91d9817db4824c60aca2668` | `c14a6bcb12f40284412b54c584bc86d424e5231fc579211008bcda547c3d5d05` |
| `api/internal/gpu/ffx_core_portability.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/api/internal/gpu/ffx_core_portability.h` | `b488f5c97a72e61c5c48b41a09274681a3eea702` | `217281792d09b88ad122055a6361a9303ccbce22238172fa6b30df7978d38968` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation.h` | `59d1d67347ffc8ba4f74f07ed18f672d77a47271` | `720ae3c8ffd4ead803128d47f56c67e66fa0d926f271794ad4c8210cbf81b010` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h` | imported + LOCAL DELTA (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h` | `67a8f9c0094d3ee6e76c14fb0a3526c80ab52dda` | `6ce9fb80aa8f85519423c100b06bf50a424c4aa567b7a91bc41a0fd9601694e7` (this tree's bytes) |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_common.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_common.h` | `afa5c5c93929e0a07bfeee738c58db38eb6e3701` | `5779ae7b19938d820ce2beb25a1d47620d4f34c330de8ec6e8c1b30da96f2533` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid.h` | `f5d351372559b984a2487ca246958fbb7dac15ea` | `b0fb4338ea5c4b15de71e22e882a5d1c48b9c4b1ac74edd49d519c39c52d8a6a` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_compute_inpainting_pyramid.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_compute_inpainting_pyramid.h` | `8895afab934d5e4a5c2ea12b9bcea6cf6aa2ad3c` | `4dfec81e9da16484c5a71e1b507cddce6ad6f8b3908bc5ebafd2a355bfef928e` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_disocclusion_mask.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_disocclusion_mask.h` | `1ee2cf93f4cbe91fd10ff92f309e6ac771dc3402` | `e156ca8598ef92fa6925bfa51932668a558692a3fe361117f3308ebe2ba87733` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_game_motion_vector_field.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_game_motion_vector_field.h` | `d67319754b5f6240b9cd7921489defddc879ad54` | `0483ed3d215cea3238a4f2b4b320bd86f2444e389b72f8844b5d23c412592dc1` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_inpainting.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_inpainting.h` | `d87d1c819f02ccdaa9f4c84121c11d08b99cede9` | `46f5a5fe4188565a2fe30a5c00c7d1996f1c4a50f751f608dc1ea026b542aa57` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_optical_flow_vector_field.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_optical_flow_vector_field.h` | `4b97a68bfeb4be253e4cd1a509caa7d4ad9a2854` | `7fde4ed3c927457a2f5ebaa18bad50e20f282e3d3622be2b3c73cb5498fa28c8` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_reconstruct_dilated_velocity_and_previous_depth.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_reconstruct_dilated_velocity_and_previous_depth.h` | `9f4de643d6ff8895b127c105cc468e2abcf4d64f` | `891192c74a20a06d5f83257f02b8b70c1837eb08e18acd113da3cc96f6962d7c` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_reconstruct_previous_depth.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_reconstruct_previous_depth.h` | `38bf02dce17f406a70990c0cf725dc33e7e1e70c` | `a88e3c02a913d174f0db13e260c03cac356db1296b8aeef45f5d23432da0faf5` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_resources.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_resources.h` | `d32049b91ec523d71c90f53ec3a2560b8a4bff61` | `f36b388fc733363fff0e0007d16ac69a9ee16eb3eb4357d9e68261208a0e2156` |
| `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_setup.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_setup.h` | `02d25786c0ceb21cd6906fe2033ac9dd2909c87d` | `2fb61ed0264534f254465e69763b190e7ff8953696fe1264c2e3a0b498ff7758` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_callbacks_hlsl.h` | imported + LOCAL DELTA (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_callbacks_hlsl.h` | `777649ca4d043e98ab36d70c72018018e7c0ec9b` | `e1f837db72cb2272c664b8c01e662f1b20ed442e2e3c23da27c501fc59ec5e8c` (this tree's bytes) |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_common.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_common.h` | `beefe5b06833573f856400bed100c7355f7a7f6b` | `8eb5ac2032c2a9b3c6bdcc9badd1034c41f9d8ffc1cb57c52ad7c726ff3b7667` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_luminance_pyramid.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_luminance_pyramid.h` | `5e5ee53414f89e9d5e3dc004b0bcf7da510a63c9` | `054f800edb3ef704e8df631d4b7eb979e25da307cd69cb6e4491b8d2caa64f8b` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_optical_flow_v5.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_optical_flow_v5.h` | `a96184e9e019624d2b090d5c276f734fad5ea748` | `513841786f7ab3cc81dd62f49851660b61f1d6d37912b4317f4401ff9fb0825b` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_scd_divergence.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_scd_divergence.h` | `9e66e8e1026bcd34f6328dc115cf2b9bd4516753` | `a4e57ff611e85b7e436e8f27fd201f9e76fcf75bdb19d22c46b64d7e414b8ce3` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_filter_optical_flow_v5.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_filter_optical_flow_v5.h` | `d26e08bf943cd9d1ed2d696b044f7c1e7386cd3b` | `acd462dfe85d1487b74c727abfaf311602fbd47c6d7a88e0b86713a50cd8904d` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_generate_scd_histogram.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_generate_scd_histogram.h` | `5492fa8bb7f73fab924b2cb8f81aa742045098b2` | `2f01747c9784055836449cc6d5ee72da6a35edea75d1840d0ec8c1928391a2c9` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_prepare_luma.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_prepare_luma.h` | `52ff3d4443b86a04f85446447abac4d92617673b` | `611da9fef5880103a49e3e0864c6ca8e39675a90137a84dacdbc387a7e761e9b` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_resources.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_resources.h` | `a17d46802fdd43de94d1c0c0e69999a1ab41eaaf` | `60abbadedb676ccf3c220dba536cac4a91fc992da235e65e72d01a3d88af05dd` |
| `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_scale_optical_flow_advanced_v5.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_scale_optical_flow_advanced_v5.h` | `9a22fa2cd2142216cce02714e94c52b5cfd10d07` | `73a40e7419edb3b4df539d5e8bf980995a9a018850bcf7db0aa37f0356dc9b3e` |
| `framegeneration/fsr3/include/gpu/spd/ffx_spd.h` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/include/gpu/spd/ffx_spd.h` | `c1ed5371ed472b76d45def59960a8ef52e5565a6` | `4d3b02d58b215f7d0d3fcdc4cabfa8f2cd03a8efdde12f770c2142b1ccc81033` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl` | `ba3ca2a5e52dbb5efe7bbbf9131b2aec7bcf4fa8` | `d77fc8b7964541ead4f949dd0aaf14f7074cb6c1353eaa4d28544bfbfb22e6d0` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl` | `3fb91cfb8d13e754df6c73477da4bc0d3c53fed1` | `6110c2e9c124cbffbfeb8ce1be2f3af659396b926e7f23087977f441940722b0` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_disocclusion_mask_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_disocclusion_mask_pass.hlsl` | `ff921ccbe825e80a49110611d0729638d9b6873b` | `f3d4fb828e41acde4231b2e84a82598c1b0478cfec9eeb246cc29c77ef7dd60c` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_game_motion_vector_field_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_game_motion_vector_field_pass.hlsl` | `92e221631f145108ae757c79685b324fb99d1331` | `24f81f154caa8fc5d16e85f88ff95c57e6efc3687cb22cd617b84fc0ceea7be1` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_inpainting_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_inpainting_pass.hlsl` | `660f31763421e272da52b79d5fda0da5ab56d779` | `a7b76063cbc089568ba479cc9338831cb23a4c090ff7740c8271baca15c18135` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl` | `f029c7e8a1abf566cdc16725939a69efc512a1eb` | `db91ea6099a46e9c741422e579a67d37ddcfc456750246d6d6666edaf3077f8a` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_pass.hlsl` | `46f6bc49ffc44749d30487776367940f12b53306` | `cb6dfea9d25e8261b8599b0cbeefe1ab82625d7219d81b14955c34c57ebe324a` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl` | `eda4db065d6083ca6a7bb54ceaad1d1e08da18c4` | `3837cb9a4189066a008fee398dd06e563d100a10b7df4a7689394b2a3b95e6bf` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl` | `0c2bb581ad281881ee31682e2aa40a7d76ac6f29` | `4d3f1e123777dd54734f469aee71487a9de1cce11da877e2247f6fcbc8e6caab` |
| `framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_setup_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_frameinterpolation_setup_pass.hlsl` | `7757963c0d64fb9b2896adbadf17241b02b18049` | `3fe17accbd59a33209893475c8e3016ca66614e370f09c4eab848a557ac36685` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_luminance_pyramid_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_luminance_pyramid_pass.hlsl` | `c7789679da66f1e1c4eca396d85c9a5b52e23b47` | `40b219141c1f76db05b21cb317fdd82473b543cc98c579d1e1535882339b050f` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl` | `27733a308c282a80bc096f4d144b0b8a0d9e4647` | `b7c96ac4f49ccac1019c07c16675ace639d16c5e09587060388a9f9ee60ef25b` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_scd_divergence_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_scd_divergence_pass.hlsl` | `3a304ef86f22873f3314e7c190dcac783c7cfc27` | `c5503f08a736542bfafffaf8c8864ab341eb4d4f7d02b6ec4cea2d31d6a7e0ce` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_filter_optical_flow_pass_v5.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_filter_optical_flow_pass_v5.hlsl` | `1e923e36347a57865fc34bd870892ee0fc5ec793` | `f7c976b33f59c62c5107c79a2af605ce6c224c557b25239d18bd82cb42b60d83` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_generate_scd_histogram_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_generate_scd_histogram_pass.hlsl` | `93e348c654ce739b108044cc073552972a8e3c4e` | `c7763a1c7967d466aa6c17e6fd4efc3f52ca940ca804f7b569ccd0b79ecb7a3c` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_prepare_luma_pass.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_prepare_luma_pass.hlsl` | `d7fec0c6feeafa51d573342addad7491b3ac6541` | `456d647ef971d55984fbe4c46ef34b696a2fb4f7256f13da1c67f6bbf15ee196` |
| `framegeneration/fsr3/internal/shaders/ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl` | imported byte-identical (2.4.0) | `Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl` | `a5a8de0eaef64dbc2554bb6fca62baabb1b14b27` | `35dde5a6a349b377f125a47fc8853b203ee01ca908cc8b5565e8a3984139d278` |
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

The same change, for the same reason, in frame generation's two callbacks headers:

- `framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h`
  (Git blob `20fd6d9ac520ce2bae8ade01fdaa12c2dfdab9a5` in this tree): six declarations,
  `rw_reconstructed_depth_previous_frame`, `rw_reconstructed_depth_interpolated_frame`,
  `rw_game_motion_vector_field_x` and `_y`, and `rw_optical_flow_motion_vector_field_x` and `_y`.
  This file has a second change. Where a pass binds both the output and the inpainting mask (the
  interpolation and inpainting passes), AMD declares `rw_output` as `RWTexture2D<FfxFloat32x3>` and
  stores three components into the output, which is in the colour format. Vulkan requires a storage
  write to have at least as many components as the image's format
  (`VUID-vkCmdDispatch-OpImageWrite-08795`), so the delta declares it `FfxFloat32x4`, stores
  `FfxFloat32x4(val.rgb, 1.f)` (alpha 1, as the upscaler's `rw_upscaled_output`), and reads back
  `.rgb`. No pass reads the output's alpha: the inpainting passes take the fourth component from the
  inpainting mask. `tools/test_fsr3_port.py` checks the component count of every storage write in the
  embedded SPIR-V.
- `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_callbacks_hlsl.h` (Git blob
  `492a7495af10bb6d83cea5723fb76ab0dbd5138d` in this tree): three declarations,
  `rw_optical_flow_scd_histogram`, `rw_optical_flow_scd_temp` and `rw_optical_flow_scd_output`.
  `rw_optical_flow_histogram` is atomic too, but no imported pass binds it, so it is left as AMD
  wrote it.

Each gets the same notice, which in the frame-interpolation header also names the output change. An
image declared R32ui is also R32ui where a pass reads it without an atomic, which matches its
`R32_UINT` view. The test checks, image by image across both sets, that an image declares R32ui
exactly when an atomic reaches it in some pass.

Upstream ends some lines in spaces, so `.gitattributes` exempts the imported files from the
whitespace checks; this manifest keeps the normal rules.

## Updating

After any change to a file here, regenerate the embeddings with the pinned compiler:
`python tools/shader_embed.py regenerate --shader-set fsr3 --dxc <DirectXShaderCompiler v1.9.2609>`,
and `--shader-set fsr3fg` for a file under `framegeneration/`.
`make fsr3-shader-verify` fails on any file whose SHA-256 no longer matches the manifest's include
closure, and `make fsr3-shader-repro-verify DXC=<dxc>` recompiles every pass and fails on any byte
difference.

# PROVENANCE.md — `third_party/rcheevos/`

rcheevos, RetroAchievements' own C library, vendored for opt-in RetroAchievements support
([#1316](https://github.com/Jstar269/nakagawa-recomp/issues/1316)). The project wrapper is
`src/core/nk_achievements.c`, which hands rc_client a bounded read of guest memory through the PSP memory map and
passes its requests to a caller-supplied transport only for `https://retroachievements.org/`. The native selftest
`tests/native/test_nk_achievements.c` (`achievements-selftest`, also run by `native-core-tests`) tests it against an
in-process fake server. The vendored files contain no network code: rc_client builds each request and hands it to the
host's callback, so nothing here opens a connection.

## Upstream source

- Project: rcheevos
- Repository: `https://github.com/RetroAchievements/rcheevos`
- Pin: release tag `v12.5.0`, commit `1433173220a7eaede6a9ed7a18e94117be1821e0` (published 2026-09-14, the newest
  release at the time of import).
- License: MIT (`third_party/rcheevos/LICENSE`, "Copyright (c) 2018 RetroAchievements.org"), kept beside the sources unchanged. MIT is
  compatible with GPL-3.0-or-later. `src/rhash/md5.c` and `md5.h` are L. Peter Deutsch's MD5 ("Copyright (C)
  1999, 2000, 2002 Aladdin Enterprises"), under the zlib licence in their own headers, which are kept unchanged;
  rcheevos uses it to sign unlock requests. The zlib licence is compatible with GPL-3.0-or-later.
- Import method: each file was read from a clone of the repository with `git show <commit>:<path>`. Its Git blob ID
  matches the pinned commit's tree, and the SHA-256 values below were calculated from the local bytes.
- Only what rc_client needs to sign in, load a game's achievement set by its hash, evaluate it and submit unlocks is
  vendored, in the upstream layout so every `#include` resolves without an edit: the public headers, `rc_client`, the
  `rapi` request builders and parsers, the `rcheevos` runtime, and `rhash`'s MD5. Not vendored: the rest of `rhash`
  (disc hashing; the hash is computed elsewhere), the editor API (`rc_api_editor`), RAIntegration and the external-client shim, the libretro
  helpers, the tests, the validator and the build files.

## File manifest

| File | Upstream path | Kind | Upstream v12.5.0 blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `LICENSE` | `LICENSE` | imported byte-identical | `5f1faf3dc4e00f66418bd1f67fb5b55f8d932faa` | `bf820c034966a3b6be050bb85f94ec841a0b972bd5d477e362337d0574b5c219` |
| `include/rc_api_info.h` | `include/rc_api_info.h` | imported byte-identical | `1894cce1231b723185130fbb6cad311d0492014c` | `5c553b7dd109e854b3d3bfbecb9fb69b56cf922c25f88d04d9fe9bb162410f2a` |
| `include/rc_api_request.h` | `include/rc_api_request.h` | imported byte-identical | `ddccc7bc28194600405008979eb8ad6ea49ced86` | `2d27109deb502ed447cb821815a04c30bec37b90b1016f805960efc056a2d9bc` |
| `include/rc_api_runtime.h` | `include/rc_api_runtime.h` | imported byte-identical | `8a0de01b7f2a3016a49c3f6a9641527fedfe4419` | `32688333ba3923695fc568acf5580805158302de85da2fc078e590ed550409f8` |
| `include/rc_api_user.h` | `include/rc_api_user.h` | imported byte-identical | `f604188aa85568814fb5b39c4bb1bdb04352a19d` | `3268cdfded32a43e78844d9e07f79f3a8be3a97a9fdfdb6a17458a566df9761f` |
| `include/rc_client.h` | `include/rc_client.h` | imported byte-identical | `dd1538c0774826c50083c15d006ae0f21297f557` | `7176b338d8a80eee3be9a188887eaf9d375eb24b19f0efedfbe6b51e3504811a` |
| `include/rc_consoles.h` | `include/rc_consoles.h` | imported byte-identical | `1625cf2174b4eed75bf46403a958aac4d45dd780` | `73c1178e501692a11fb702eb1d90d25a7c47ed1d02678c619bc087c9755b132b` |
| `include/rc_error.h` | `include/rc_error.h` | imported byte-identical | `197bcea7023dfada97ae83eaa984830b7b43b797` | `b8d8214fe7ed4c0bb86420f1bfc4c4cb63548907b2c1001fea2083a55554763a` |
| `include/rc_export.h` | `include/rc_export.h` | imported byte-identical | `9471acd480b2fa61a5f97ee1d93741e5fce45603` | `9683f979bb152781677ffd952a68fdddcb5d951eb227e96a2d1f7fd0feb3be62` |
| `include/rc_hash.h` | `include/rc_hash.h` | imported byte-identical | `be1d37812394d8ae9449cf135d531e5807a04139` | `b2eeb7eb928ed07094463eefd8111ecc69a2f8aeb04e63b23172dcf3663a271b` |
| `include/rc_runtime.h` | `include/rc_runtime.h` | imported byte-identical | `88b12dea27d77c46e39931f7b0843c2c57ec5c61` | `a3c5e73681844036bc9d343e82fbc0d79c8bb6b6c584044b200515ac025a3063` |
| `include/rc_runtime_types.h` | `include/rc_runtime_types.h` | imported byte-identical | `e17797063c0de25515c072c74b2d18b21ddad146` | `f7d63f647773e4dc251ae8dc577d15a329093715c3c8b5172fb5b652d099c416` |
| `include/rc_util.h` | `include/rc_util.h` | imported byte-identical | `18d27e0fc789f19a19920061502e397151e87449` | `12840744dcca64ffd2e5f0a1c92fca2e235236edd5a66d8787d1d26189254344` |
| `include/rcheevos.h` | `include/rcheevos.h` | imported byte-identical | `0d96002148b69653ba6d49c1b66c503a985a2f43` | `0264f2a35a7f79f144263384d5378976c4c1062fa7477a281ed6fe2d64eba760` |
| `src/rapi/rc_api_common.c` | `src/rapi/rc_api_common.c` | imported byte-identical | `3b3528605b225e3d1dc14515b2dbc4190d4507f5` | `972ab73888c2bf37efde05e3daca487cdaf1e47735abe0ada3d48a4a9cb8c128` |
| `src/rapi/rc_api_common.h` | `src/rapi/rc_api_common.h` | imported byte-identical | `06a7485da35a1121c703d9f460564cd8391c6db6` | `f6c49b5c6ff67e1b3c28d108799a6c975d2a7340b399e7a92cf0dbbaea60267b` |
| `src/rapi/rc_api_info.c` | `src/rapi/rc_api_info.c` | imported byte-identical | `327a6dff3fc8f775f0bf369205c11482e43f9638` | `d9a3707339b36052672eca0a0195487666a0866ff1c428cec69508bb5cef5f5c` |
| `src/rapi/rc_api_runtime.c` | `src/rapi/rc_api_runtime.c` | imported byte-identical | `261741d55aeebf4323b126f993dbf53483529c83` | `5ce6ea89d8f4bc4cfbf492ef2553323f7f0f043dd2e5a0a45588ca0661f44ae2` |
| `src/rapi/rc_api_user.c` | `src/rapi/rc_api_user.c` | imported byte-identical | `6943c98c443d647070e637758184fc4c09bec499` | `2430718f9fce10d4aa9f887b3cafdb817c37164291983fc345c19dac8c2ccfe2` |
| `src/rc_client.c` | `src/rc_client.c` | imported byte-identical | `7a7399580ebe48fd92b84d3a198f51b6675a23fd` | `f3fcc544f082acef62ca32fdd8d05f43fed5f77e14484c344e8dca9402e0024f` |
| `src/rc_client_external.h` | `src/rc_client_external.h` | imported byte-identical | `b3c0e5eba9bfa12f32cc54bf6150bf67d67b6da9` | `dbca667b2ec79cc74347d234085b7c9fb3bd931e22bc2d598fb0af9931edc3ba` |
| `src/rc_client_external_versions.h` | `src/rc_client_external_versions.h` | imported byte-identical | `90f6f689604498d8d9ec61f6126c6c13f7f7328d` | `47c3a1975907b4d77ae7f427257f15cead2a9eef95c32b6034ebd3c048a330f1` |
| `src/rc_client_internal.h` | `src/rc_client_internal.h` | imported byte-identical | `fad288d62aee1717d4062bb2661ef5b5c9075a5a` | `a7fa88dbad2cb390cf84a5b2697284c928ef74fd4438afa81e6e633cbd8aacd2` |
| `src/rc_compat.c` | `src/rc_compat.c` | imported byte-identical | `4658c18d08e0f2fc8542074d81070bfae1a9939e` | `13869045a2fc3b33c0161dafc9e37d870be66b6642c5c0917b896c7b11a599fd` |
| `src/rc_compat.h` | `src/rc_compat.h` | imported byte-identical | `3b788e2e771763f20fc4dbdc3f2786bd80fd1687` | `4a97999341f386ef1a3b1271b1696e69dd476b0b073a752c7868c5420d9dabc5` |
| `src/rc_util.c` | `src/rc_util.c` | imported byte-identical | `3efdf04da480707da81c0a5c34f6045bbbd5a350` | `8e7a44443c9062054ee97bb9c60148ea7f14116a42d525d8abe3835a0a5c9666` |
| `src/rc_version.c` | `src/rc_version.c` | imported byte-identical | `a29d0a5f522f6b474063056cfd926a04dd49aef8` | `210091300446cf20bcbfc8f80c747c543a5a99e143887cf6aff16ab5f5a245a3` |
| `src/rc_version.h` | `src/rc_version.h` | imported byte-identical | `a2c0544d929e060daf2410a36efab82c653ffb0e` | `99db64703fd642b32812efe3b6fefb4843a2f1da6352aed3cb0d89cefcaafe0d` |
| `src/rcheevos/alloc.c` | `src/rcheevos/alloc.c` | imported byte-identical | `96fb8e382511670b8bcccb3197caf9260927ce49` | `aac9433815af40f2ffaba468f251304bb446e27cf54087982ec80ad358a12829` |
| `src/rcheevos/condition.c` | `src/rcheevos/condition.c` | imported byte-identical | `eb57a1cba3ea6add393b6b557775f0786b6a235d` | `bf21adfc2be390f9ba5a85c4039b73645a866e4100139368541efdd654bafe90` |
| `src/rcheevos/condset.c` | `src/rcheevos/condset.c` | imported byte-identical | `c9f984bfffd8d6bb4d248c819689165c25b020a5` | `e619f132a9a2bd62bc9d246a8e7598682806f8446eb4b197c09fb2929f65859b` |
| `src/rcheevos/consoleinfo.c` | `src/rcheevos/consoleinfo.c` | imported byte-identical | `2acedfdec644c855c99f763f129e2649ac745ce3` | `8e2949ee957152c1989c97472634d4a12859feaaa8c89c205775434a734d006d` |
| `src/rcheevos/format.c` | `src/rcheevos/format.c` | imported byte-identical | `bfeadf44cf5415044bc4d018cd19d20a0679ff10` | `1deaa114504049b9f303b4a01087f1639ff7939e6729671b3677106e33f8c59d` |
| `src/rcheevos/lboard.c` | `src/rcheevos/lboard.c` | imported byte-identical | `84ff5460af5a72888b0f16150b33b69969a214f0` | `14d94ba718d3e694e4874bbcead3c32fb0141728ef4fe011646d266b9261249e` |
| `src/rcheevos/memref.c` | `src/rcheevos/memref.c` | imported byte-identical | `577c7e483336a32a367a6b03db75edfe87e14967` | `4a1f0a044efc4a1eb2da11f484a630ef8169b86e9b53a193d9439d35f5d9c801` |
| `src/rcheevos/operand.c` | `src/rcheevos/operand.c` | imported byte-identical | `5d9c81a5cfd76ba151939bc83d0e798b84b5bb6a` | `b787371db68a52bb17fe6a0199630287cfd0314e51362c93afaaa89592d6b4c8` |
| `src/rcheevos/rc_internal.h` | `src/rcheevos/rc_internal.h` | imported byte-identical | `51e2e234c9095fe49be755a0243050592f24f0b0` | `bdabc324f20606e6377e86b606d587d0ebe061f7413c2afce0a850b7274d4b8b` |
| `src/rcheevos/rc_validate.c` | `src/rcheevos/rc_validate.c` | imported byte-identical | `843c2ad30cf77f4aecbc7651eb74ddaf2a19e007` | `b669159309b67eecbfd9ddd2332cdc083e025f75bb743f6e5c972f7ee5cdee0d` |
| `src/rcheevos/rc_validate.h` | `src/rcheevos/rc_validate.h` | imported byte-identical | `cfda94075d36295b3a6cb78e4ad9f239eb507774` | `d3da5219ebf38d2c34008417b3415658997cb3b63ee26b859d4036fd3fff3191` |
| `src/rcheevos/richpresence.c` | `src/rcheevos/richpresence.c` | imported byte-identical | `c6a3393e2a22f29c9119e27c5725e7eafdf4dc60` | `06c940a6159c9fb39785b29eed204865150cef89431bd923d4f11465665a1144` |
| `src/rcheevos/runtime.c` | `src/rcheevos/runtime.c` | imported byte-identical | `c1100b9af9a9c33a50ba75420b51704d36d6336d` | `5dd68234cecb9111646431bc785a07eaca3234e8794a9c25d0622fe1607a360f` |
| `src/rcheevos/runtime_progress.c` | `src/rcheevos/runtime_progress.c` | imported byte-identical | `c87721eb4fc04df5a110400a923bf7dec567681c` | `24de7caf7edf30fc54a65314c5046389be138d11ff122d20d70845fb778439ce` |
| `src/rcheevos/trigger.c` | `src/rcheevos/trigger.c` | imported byte-identical | `063a14f5c92c8e924c34a6325adf5d69a5340737` | `fe38429dd6c0e18d2c27c1eb797c963d61f63130c6546a20e1f8d27ed8271ece` |
| `src/rcheevos/value.c` | `src/rcheevos/value.c` | imported byte-identical | `9a451c63c88d74cb6e540bc286712394f31fe41f` | `126a5e14182fa551c03c758a2066feeda17c6e255a0dcb21fc6895ed5371413b` |
| `src/rhash/md5.c` | `src/rhash/md5.c` | imported byte-identical | `f3a52056695d3b582c3738f5e839a38db0547cbd` | `fd38ee6a43316007d4aa816f23c24a8355c58df106b4c54e43ec44afc55df521` |
| `src/rhash/md5.h` | `src/rhash/md5.h` | imported byte-identical | `698c995d8f4e9d98766d568fe88863381f94d16f` | `8341e2a307d7cfaeb816b261d92ce87a07be62e3eb58c98d2c769e75a6e2b959` |
| `PROVENANCE.md` | n/a | project-authored manifest | n/a | n/a |

## Local deltas

None. No source file is edited. The bridge builds rc_client without `RC_CLIENT_SUPPORTS_HASH`,
`RC_CLIENT_SUPPORTS_EXTERNAL` and `RC_CLIENT_SUPPORTS_RAINTEGRATION`, rcheevos' own defaults, so the files those
switches would pull in are not needed.

# PROVENANCE.md — `src/rt/h264_ffmpeg_sub/`

Portable H.264 video decoder for the Nakagawa recomp runtime (PSMF movies on Linux, later
Android; Windows keeps Media Foundation). The decoder is FFmpeg's libavcodec H.264 decoder,
imported byte-exact together with the libavcodec/libavutil framework files it needs, built
from generic C only. A thin C API written for this project (`nk_h264_api.h`) wraps it.

Decision: maintainer, 2026-10-24, on issue #283 (pinned FFmpeg H.264 subset, following the
`src/rt/atrac3p/` pattern, as PPSSPP does).

## Upstream source

- Project: FFmpeg
- Repository: `https://git.ffmpeg.org/ffmpeg.git` (upstream canonical)
- Pin: tag `n4.4`, the same pin as the ATRAC3/ATRAC3+ subset in `src/rt/atrac3p/`. The tag
  peels to commit `dc91b913b6260e85e1304c74ff7bb3c22a8c9fb1` (2021-04-08), tree
  `180278cb712fb2d247b27895fb43ac56ae905cf1`. Every blob SHA below is resolved as
  `n4.4:<path>` against that commit.
- License: LGPL-2.1-or-later (see `LICENSE.LGPLv2.1.txt`, imported from `COPYING.LGPLv2.1` at
  the same pin; upstream blob `58af0d3787aec7d6f8d833535d64879156e7ec23`).
- Import method: files extracted byte-exact with `git show n4.4:<path>` from a clone at the
  pin. The file set is not hand-picked: FFmpeg was configured out of tree with
  `--disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avformat
  --disable-avfilter --disable-swscale --disable-swresample --disable-postproc
  --disable-network --disable-asm --disable-pthreads --disable-autodetect
  --enable-decoder=h264 --enable-parser=h264`, a program using the decode API
  (`avcodec_find_decoder`, `avcodec_open2`, `avcodec_send_packet`, `avcodec_receive_frame`)
  was linked against the resulting static libraries, and the archive members the linker pulled
  in (72 translation units) plus every header and included template those units read
  (from FFmpeg's own dependency files) are the 208 files below.

## What is deliberately not here

No assembly or SIMD (`libavcodec/x86`, `arm`, `aarch64`, ...), no hardware acceleration
(VAAPI, VDPAU, NVDEC, DXVA2, D3D11VA, VideoToolbox), no threading (`pthread*.c`), no parsers,
no other codec, no encoder, no demuxer, no libavformat, libswscale or libswresample. The H.264
decoder runs its C DSP paths, so output is the reference bit-exact result on every host.

## File manifest

Legend:

- `imported byte-identical` — working-tree content hash (`git hash-object`, LF checkout) equals
  the upstream n4.4 blob hash. There are no `imported + LOCAL DELTA` files: the build is
  adapted by the authored files below and by compiler flags, not by editing upstream.
- `AUTHORED` — written for this project. `n/a` upstream counterpart: no such file exists in
  the n4.4 tree (configure- or script-generated files and the API layer).

### Authored files

| File | Kind | Upstream n4.4 blob SHA |
| --- | --- | --- |
| `config.h` | AUTHORED stand-in (configure output) | n/a |
| `libavutil/avconfig.h` | AUTHORED stand-in (configure output) | n/a |
| `libavutil/ffversion.h` | AUTHORED stand-in (version.sh output) | n/a |
| `libavcodec/codec_list.c` | AUTHORED stand-in (configure output) | n/a |
| `libavcodec/bsf_list.c` | AUTHORED stand-in (configure output) | n/a |
| `nk_h264_symbols.h` | AUTHORED (symbol namespace) | n/a |
| `nk_h264_api.h` | AUTHORED API layer | n/a |
| `nk_h264_api.c` | AUTHORED API layer | n/a |

### Imported files (209)

| File | Kind | Upstream n4.4 blob SHA |
| --- | --- | --- |
| `compat/va_copy.h` | imported byte-identical | `a40bbe663716b488a4eb943f3a2dbd5604c9ae29` |
| `libavcodec/allcodecs.c` | imported byte-identical | `2e9a3581dea22510a5661bbd5937360a52ff49de` |
| `libavcodec/atsc_a53.c` | imported byte-identical | `2d89ef50b94ce761c69183b8b3e13acfaf3f8948` |
| `libavcodec/atsc_a53.h` | imported byte-identical | `0622a5554937cf6e27e6d64b7c090c708f2cab03` |
| `libavcodec/avcodec.c` | imported byte-identical | `e572e9533ddc181886da3dc3c6cdf12d16476b3a` |
| `libavcodec/avcodec.h` | imported byte-identical | `8a71c042308e37a89dbf357d8f9b276dd4ebcbf9` |
| `libavcodec/avpacket.c` | imported byte-identical | `b5bac5c5f282787c8af357d59f8b4819713aceff` |
| `libavcodec/bit_depth_template.c` | imported byte-identical | `d44d47ea456be2f5cbb5c02b1588e4edaa8ef55b` |
| `libavcodec/bitstream.c` | imported byte-identical | `e425ffdc96473034c78c4844ba8be3234189f93c` |
| `libavcodec/bitstream_filters.c` | imported byte-identical | `02d33abb18b3df84824e982233ee36b3e1b665c7` |
| `libavcodec/blockdsp.h` | imported byte-identical | `26fc2ea13b6ad38d7e97c727cccbc26a885c3b8f` |
| `libavcodec/bsf.c` | imported byte-identical | `157984bd9a24892b5debdd52405504c907bd7332` |
| `libavcodec/bsf.h` | imported byte-identical | `3b5faa85cb513cc055658edbadc012aaa22d89ed` |
| `libavcodec/bsf_internal.h` | imported byte-identical | `b78c134bdda61345f6e4d13a11c0cb83cd613344` |
| `libavcodec/bytestream.h` | imported byte-identical | `d0033f14f3690d6cedf1320b82d42f4d59124770` |
| `libavcodec/cabac.c` | imported byte-identical | `6649feebeb9b36ec1a1efd9d674d1648d04ca524` |
| `libavcodec/cabac.h` | imported byte-identical | `38d06b2842e89a8070f11059697b4b097d979213` |
| `libavcodec/cabac_functions.h` | imported byte-identical | `46af92182212c4eb5a690681690841fd00c1492b` |
| `libavcodec/codec.h` | imported byte-identical | `50a22f6e3cb0a5d068c6a2cea07de4fdebe9123a` |
| `libavcodec/codec_desc.c` | imported byte-identical | `17f8a140446e64972ffa000cbeb007182f737800` |
| `libavcodec/codec_desc.h` | imported byte-identical | `126b52df47629c51816b510cf9ee5c67f6f0f498` |
| `libavcodec/codec_id.h` | imported byte-identical | `ab7bc68ee2c43741be7ec9b88d51bd860e41c444` |
| `libavcodec/codec_par.c` | imported byte-identical | `1a5168a04b8da0be215dc1442a37a0e7fd90b0b4` |
| `libavcodec/codec_par.h` | imported byte-identical | `948758e2373d191cf93c1211fb87c85bdee16da3` |
| `libavcodec/copy_block.h` | imported byte-identical | `393d45578e78c5bc4a4bcb36471d386684e121e3` |
| `libavcodec/decode.c` | imported byte-identical | `5a00aeedaefb4cc5f5c395b2120743e698e60874` |
| `libavcodec/decode.h` | imported byte-identical | `1467b1eb33788fed99b36cf0ec7ad7a75d231a4c` |
| `libavcodec/encode.c` | imported byte-identical | `89df5235dafdd56a5f5154cbebc50a2f65dfeeb6` |
| `libavcodec/encode.h` | imported byte-identical | `9af2f42483fd2c497ce385bc6207a68889f092e4` |
| `libavcodec/error_resilience.c` | imported byte-identical | `ca2287198be69505a03f12dcbe9cff2b485de769` |
| `libavcodec/error_resilience.h` | imported byte-identical | `664a765659272a214b9807de2584c756a03f3228` |
| `libavcodec/fdctdsp.h` | imported byte-identical | `3e1f683b9eab136463b979d232cdeb834cf7eb23` |
| `libavcodec/frame_thread_encoder.h` | imported byte-identical | `c400d6b32c80a113fcd32c033a720e3d6592028d` |
| `libavcodec/get_bits.h` | imported byte-identical | `66fb8775994b3a9fee3fac9125981bccfce7aa68` |
| `libavcodec/golomb.c` | imported byte-identical | `937ac22ce13452ce9a9f84105a84c6719ddd3eb0` |
| `libavcodec/golomb.h` | imported byte-identical | `4d531cf8053c598a41480bc9445c336fcd62694c` |
| `libavcodec/h263dsp.h` | imported byte-identical | `1abea3ca8cd3227d8cb0d7acee80816b4145601d` |
| `libavcodec/h264.h` | imported byte-identical | `7a1fb6d68798a507da916950e21059f65e3b44de` |
| `libavcodec/h2645_parse.c` | imported byte-identical | `6fbe97ad4aac237f01a6b8f629212be6c2d1fb27` |
| `libavcodec/h2645_parse.h` | imported byte-identical | `3e47f86c53b2f993983d9418e94c99e51a2782d4` |
| `libavcodec/h264_cabac.c` | imported byte-identical | `86f0a412faa738ae2997f518345578252eb9e25e` |
| `libavcodec/h264_cavlc.c` | imported byte-identical | `9f5f692331ca4ac529431850d5de88e4314327fc` |
| `libavcodec/h264_direct.c` | imported byte-identical | `8f079811303c67daac0d601774a623696964d052` |
| `libavcodec/h264_loopfilter.c` | imported byte-identical | `558ec6c02dfb3b1d2e3dfaf59a4b02c5de88112f` |
| `libavcodec/h264_mb.c` | imported byte-identical | `3cd17b7e4bafdd3f62670ca7686051471fa7d1b6` |
| `libavcodec/h264_mb_template.c` | imported byte-identical | `d5ea26a6e3fc21a2a25e552cc00b7a20cabbde58` |
| `libavcodec/h264_mc_template.c` | imported byte-identical | `d02e2bf580a409aec4a9a88c05c37c2a6cc9e1a3` |
| `libavcodec/h264_mvpred.h` | imported byte-identical | `19d9ee462d469ebf8fafbfc554a8b5386f2bcce1` |
| `libavcodec/h264_parse.c` | imported byte-identical | `1c1d1c04b02ba4fa756da8445e2f2f0e6f597f42` |
| `libavcodec/h264_parse.h` | imported byte-identical | `4d016201256cad92563ae82775cb8c9c02709115` |
| `libavcodec/h264_picture.c` | imported byte-identical | `eec5e9fb9a0a85cdc361caf7355d79217baec842` |
| `libavcodec/h264_ps.c` | imported byte-identical | `e21c2b56acf4bb199f4452da1e8428cfd0edadfe` |
| `libavcodec/h264_ps.h` | imported byte-identical | `3f1ab72e385a8fd8f1528cb95701c04b7457e9be` |
| `libavcodec/h264_refs.c` | imported byte-identical | `dae8bd278adbe7ab14cbcb64a308ba71f12bbcc3` |
| `libavcodec/h264_sei.c` | imported byte-identical | `ca2ca59e1f5482c59b7d302ffd6ad0b1f757d848` |
| `libavcodec/h264_sei.h` | imported byte-identical | `14cc559361d614a4e943e9b47fa9685155c28874` |
| `libavcodec/h264_slice.c` | imported byte-identical | `62f7a61aed9af051c78ee6202f386c999c1dbd4b` |
| `libavcodec/h264addpx_template.c` | imported byte-identical | `9a1e6a2f2f603270b9b32a5c12c0108412a329d4` |
| `libavcodec/h264chroma.c` | imported byte-identical | `c2f1f30f5a922bfdf2fa41e7bd7f97da6499653f` |
| `libavcodec/h264chroma.h` | imported byte-identical | `5c89fd12df61bfce89d3acd481d19367669a73b7` |
| `libavcodec/h264chroma_template.c` | imported byte-identical | `a3ca07b518f3881741524bfa0a4a590f89d8a00d` |
| `libavcodec/h264data.c` | imported byte-identical | `a4c6d93cf0b5bd64089d9928f816f1305a5876c8` |
| `libavcodec/h264data.h` | imported byte-identical | `2968b08b7e9d0e93f9bf521191da88b8fe0f0244` |
| `libavcodec/h264dec.c` | imported byte-identical | `47b9abbc5c384044f0f4d0a3896ab7658a74df87` |
| `libavcodec/h264dec.h` | imported byte-identical | `b3677cdbb9edc81fc597b7d4a4457fabf98abc64` |
| `libavcodec/h264dsp.c` | imported byte-identical | `d26f552369d0cdca7f8ce25dae3c11ed3c2ba6da` |
| `libavcodec/h264dsp.h` | imported byte-identical | `cbea3173c6a301f5d9dd26890216e1b0b0819350` |
| `libavcodec/h264dsp_template.c` | imported byte-identical | `fe23a2cff1f1aff3d5c76a55af987ebdaeaa3bf2` |
| `libavcodec/h264idct.c` | imported byte-identical | `6a771affe1cedbd0f4c37b3c1e2f129339ee1c5b` |
| `libavcodec/h264idct.h` | imported byte-identical | `17e0051497b6ecf221a2439563539e88958424aa` |
| `libavcodec/h264idct_template.c` | imported byte-identical | `ce66ed3ab889ad6756fb131993f58d4383f0bd41` |
| `libavcodec/h264pred.c` | imported byte-identical | `5632a58fd767523473dfccf7347548a83ab529a2` |
| `libavcodec/h264pred.h` | imported byte-identical | `2863dc9bd1945e732df21b161b3835e42dd3f834` |
| `libavcodec/h264pred_template.c` | imported byte-identical | `2b30fff70f5c593a4dff6b2d51689e45e53ad5ae` |
| `libavcodec/h264qpel.c` | imported byte-identical | `50e82e23b080c504cbdd508e8c66a54bc46c23a5` |
| `libavcodec/h264qpel.h` | imported byte-identical | `7c57ad001c7787fbe6c456030c11faed52e6b207` |
| `libavcodec/h264qpel_template.c` | imported byte-identical | `27c5b8f17fea3154b8b44883361693e9645e2977` |
| `libavcodec/hevc.h` | imported byte-identical | `1804755327e134075d06891811d5ca435bed656d` |
| `libavcodec/hpel_template.c` | imported byte-identical | `fccfe7610fe581c1b7b5f5d9d6e90705988fecad` |
| `libavcodec/hpeldsp.h` | imported byte-identical | `768139bfc96e248e3a2c2cc608375c6806c4e509` |
| `libavcodec/hwaccels.h` | imported byte-identical | `8e54cf73f90f7564af4d8d5682786bab77018893` |
| `libavcodec/hwconfig.h` | imported byte-identical | `f421dc909f4441a615382e003e7e379a5c2e3532` |
| `libavcodec/idctdsp.h` | imported byte-identical | `ca21a31a028cebf20977f3767166d3a2cc9d64f2` |
| `libavcodec/internal.h` | imported byte-identical | `b57b9968166f9032baa06ff84e9426b493a8fee5` |
| `libavcodec/mathops.h` | imported byte-identical | `1c35664318f8ba8753d7586c876a4611f6f213cd` |
| `libavcodec/mathtables.c` | imported byte-identical | `81eabc7a654591399f0c3d4fa8835395c652dde2` |
| `libavcodec/me_cmp.c` | imported byte-identical | `60545248b3a4a7db66de097a8469cb29a32e0ddf` |
| `libavcodec/me_cmp.h` | imported byte-identical | `e9b5161c9ad067d97947ef94c72da7871db9a312` |
| `libavcodec/motion_est.h` | imported byte-identical | `5742e51486ce7035e9a00efacdfe70baae5d74a4` |
| `libavcodec/mpeg12data.h` | imported byte-identical | `f51faf460744bfab62a1c5ae57c3bd58da65436e` |
| `libavcodec/mpegpicture.h` | imported byte-identical | `4bcd666797980ae4804408b48ddbee7999139ae9` |
| `libavcodec/mpegutils.h` | imported byte-identical | `81f0b73bb153dada07f82659dedd7b9374c23b69` |
| `libavcodec/mpegvideo.h` | imported byte-identical | `5a99c196561d5831b7b9f90878bcfb9bec33ecab` |
| `libavcodec/mpegvideodata.h` | imported byte-identical | `14f4806d66992794d41a8561a662e0ccdc439307` |
| `libavcodec/mpegvideodsp.h` | imported byte-identical | `293e2548d34c75047306cd60931442a9c2e9b3f3` |
| `libavcodec/mpegvideoencdsp.h` | imported byte-identical | `33f0282fcc3fed5d55e1d3f4bf2da684423fce23` |
| `libavcodec/null_bsf.c` | imported byte-identical | `595945027d008d2064bed6f875c02eec7b09b21b` |
| `libavcodec/options.c` | imported byte-identical | `61689b48d94e561cf703f7cbd8ef4b05a06342af` |
| `libavcodec/options_table.h` | imported byte-identical | `e12159f734514f83297c01c101f0dd3d596f5222` |
| `libavcodec/packet.h` | imported byte-identical | `ca18ae631f362476109d931264da80f4d23e4797` |
| `libavcodec/packet_internal.h` | imported byte-identical | `b1d91f634725c644a71a8d5685c359940f0b8082` |
| `libavcodec/parser.h` | imported byte-identical | `ef35547e9b65409b7ddad2e27c2a49add5d23f3b` |
| `libavcodec/pel_template.c` | imported byte-identical | `6da7a56b2dacb3a39abfab722e7b3b23d5efa483` |
| `libavcodec/pixblockdsp.h` | imported byte-identical | `07c2ec4f406f3c72d88bb2a9595829d10592dd0a` |
| `libavcodec/pixels.h` | imported byte-identical | `98eacd4df61228a2dd197921d6e3b82ab58a5438` |
| `libavcodec/profiles.c` | imported byte-identical | `7af7fbeb13026a5b0ebba8aab69b7b45644e5594` |
| `libavcodec/profiles.h` | imported byte-identical | `41a19aa9add43126785faddf117a9055cb552079` |
| `libavcodec/put_bits.h` | imported byte-identical | `f07944a8fbee01053e761916bb82e5ac756bf8c6` |
| `libavcodec/qpeldsp.h` | imported byte-identical | `91019eda9c5d0afb4f8a93d69cd58df932854766` |
| `libavcodec/ratecontrol.h` | imported byte-identical | `2a7aaec644c426235ff05d2144a49a081aaaa185` |
| `libavcodec/raw.h` | imported byte-identical | `28a27b1f9e8d7c83a34136b4d8e001c0273b5518` |
| `libavcodec/rectangle.h` | imported byte-identical | `df7c18a4e2fe920b720c765f4056c58bfbcdd2fa` |
| `libavcodec/rl.h` | imported byte-identical | `26e0b32a907a1baa2e1b369aeab7bb3600c67ca4` |
| `libavcodec/rnd_avg.h` | imported byte-identical | `344775e31fda1ea868a0e8963a199bb1d8f8b908` |
| `libavcodec/sei.h` | imported byte-identical | `5513590b51e52e938fefa22ee0a12f986422821a` |
| `libavcodec/simple_idct.c` | imported byte-identical | `0ef167491a1c7f11301f54b77943a0d2a2609365` |
| `libavcodec/simple_idct.h` | imported byte-identical | `20578b33477b1d8626607c3ad6cb52921cd6a912` |
| `libavcodec/simple_idct_template.c` | imported byte-identical | `5ddd0b45a2b717628dfa0505bae4c7b6e8d647ba` |
| `libavcodec/startcode.c` | imported byte-identical | `9efdffe8c6628e2d0e491f8cb641d56ad266c5db` |
| `libavcodec/startcode.h` | imported byte-identical | `cfa02b08602eb45e6edbb1d226f14e23cd343059` |
| `libavcodec/thread.h` | imported byte-identical | `413196269f59dc007b3db4a0f36e8c267ecce3b0` |
| `libavcodec/utils.c` | imported byte-identical | `896b99dc3f2eb31b4e848e942822411849d2e466` |
| `libavcodec/version.h` | imported byte-identical | `cfdde469600bd30a6d60cfe8816747dba6edbd2b` |
| `libavcodec/videodsp.c` | imported byte-identical | `ce9e9eb1439d8c134f12caef79daad82d94c37f6` |
| `libavcodec/videodsp.h` | imported byte-identical | `c0545f22b02789425d956c7989540fa98433c9ca` |
| `libavcodec/videodsp_template.c` | imported byte-identical | `55123a5844689607c95aed73d9c0b4ee5633d925` |
| `libavcodec/vlc.h` | imported byte-identical | `6879c3ca6a7c29a3b1e8a87f13a849aac1aa8b0a` |
| `libavutil/attributes.h` | imported byte-identical | `5cb9fe345288e1e5a892a454cc6fbc0ae7883cf5` |
| `libavutil/avassert.h` | imported byte-identical | `9abeadea4a2319832491e57d6b4ab16da2e738f5` |
| `libavutil/avstring.c` | imported byte-identical | `49e8df55aa3bc24d58bccd3c54556114855b4f31` |
| `libavutil/avstring.h` | imported byte-identical | `fae446c3027449595a56d44375b5b9a6671c1005` |
| `libavutil/avutil.h` | imported byte-identical | `4d633156d14df32518281d0a9750de52bf0c69fb` |
| `libavutil/bprint.c` | imported byte-identical | `e12fb263fe791090f013d50b9b9f53a5ca59d054` |
| `libavutil/bprint.h` | imported byte-identical | `c09b1ac1e1aa77686780694e7f45c4ad59b7dc5d` |
| `libavutil/bswap.h` | imported byte-identical | `91cb79538dc2fb5979df88ce5e8279ec80170600` |
| `libavutil/buffer.c` | imported byte-identical | `858633e8c73b6f1587fa1ea6bc072e9a65f9dd41` |
| `libavutil/buffer.h` | imported byte-identical | `241a80ed67090fc7445b4643c28b4feded70e2c7` |
| `libavutil/buffer_internal.h` | imported byte-identical | `d902772ed3ebfc024c8ec3c462a4ac1702b8229d` |
| `libavutil/channel_layout.c` | imported byte-identical | `ac773a9e634c18f6e46e43a6c355ee521f163946` |
| `libavutil/channel_layout.h` | imported byte-identical | `d39ae1177afea3e58e2396711b5d02aa8985094e` |
| `libavutil/common.h` | imported byte-identical | `aee353d3993d1ef725eeb865dde33141eb183cbe` |
| `libavutil/cpu.h` | imported byte-identical | `83099dd9697308671ee318cc955a1558d889c7e2` |
| `libavutil/dict.c` | imported byte-identical | `9d3d96c58b64c78a9f5ad0f5849665827d7bd0cf` |
| `libavutil/dict.h` | imported byte-identical | `118f1f00ed2046fa5ae5a2440a5961190d923e3a` |
| `libavutil/display.c` | imported byte-identical | `a0076e067bb3152fa4d12658c919b63d564c2152` |
| `libavutil/display.h` | imported byte-identical | `515adad795d529904dbf300cdd79e6356c742fa1` |
| `libavutil/dynarray.h` | imported byte-identical | `3a7e146422a13ffacaa539646f69a0aa59da5ef9` |
| `libavutil/error.c` | imported byte-identical | `b96304837bb7685841a01da3c8100ec90b087bde` |
| `libavutil/error.h` | imported byte-identical | `71df4da353b9cd95e45e86c14acc0c285854f0af` |
| `libavutil/eval.c` | imported byte-identical | `d0bada9b3181e07ba746d35693b8722f11743fef` |
| `libavutil/eval.h` | imported byte-identical | `068c62cdab03ce5dfece0418d89a18c2b93fb10c` |
| `libavutil/ffmath.h` | imported byte-identical | `aad1347f26d4110acccc36f498b8eace69e95f6c` |
| `libavutil/fifo.c` | imported byte-identical | `1060aedf135182befc3c7c6df51e50b56304c443` |
| `libavutil/fifo.h` | imported byte-identical | `dc7bc6f0dd7925a6c0e94847d537fa62164a4639` |
| `libavutil/file_open.c` | imported byte-identical | `cc302f2f768d0b92df3591ebc1f02715f41f7e8a` |
| `libavutil/frame.c` | imported byte-identical | `75e347bf2f0a831d495186c3a8a5296c7a840c2f` |
| `libavutil/frame.h` | imported byte-identical | `7d1f8e293579c491868a9be181bd8ddb5962ad96` |
| `libavutil/hwcontext.c` | imported byte-identical | `d13d0f7c9bc294d5164f830d5b3cc23655355be2` |
| `libavutil/hwcontext.h` | imported byte-identical | `04d19d89c2b8567788b95d8faa82a9af58618e2f` |
| `libavutil/hwcontext_internal.h` | imported byte-identical | `e6266494ac73cb825d8e24970268382673865bf1` |
| `libavutil/imgutils.c` | imported byte-identical | `bd1333170acea85e46cdb20f1fecaed8031468bb` |
| `libavutil/imgutils.h` | imported byte-identical | `5eccbf0bf7871e7a31e998501372b31e67a536ae` |
| `libavutil/imgutils_internal.h` | imported byte-identical | `d5158584133ba0c7443dba6fc74820dd9ff8e029` |
| `libavutil/internal.h` | imported byte-identical | `2ed1c2abb167fa8ad6d3f2157b41eefaf611d614` |
| `libavutil/intfloat.h` | imported byte-identical | `fe3d7ec4a5b6f5f616c103aef5a5cf7ec01f8242` |
| `libavutil/intmath.h` | imported byte-identical | `9573109e9d1b910496cc280815883a011e96feb2` |
| `libavutil/intreadwrite.h` | imported byte-identical | `4c8413a536868b9a272d1030a20d0d776880b1d8` |
| `libavutil/libm.h` | imported byte-identical | `a8199623912d50d3439cb11e59c0c9a7712d125d` |
| `libavutil/log.c` | imported byte-identical | `66defa9c424b5f8ca4351145266f4c445dced59b` |
| `libavutil/log.h` | imported byte-identical | `8edd6bbf2b8bdd21dd711565fc1b4ac51e160d44` |
| `libavutil/log2_tab.c` | imported byte-identical | `0dbf07d74c5e2fbbb2acb0fc99196b62753af8d3` |
| `libavutil/macros.h` | imported byte-identical | `2007ee5619871558dd07cd516ca77324dcf75940` |
| `libavutil/mathematics.c` | imported byte-identical | `2de2b39da0892e7f2e8acde1f4eaee3c42e99af4` |
| `libavutil/mathematics.h` | imported byte-identical | `54901800ba6ad22fd33d6c8595400d378c7429ca` |
| `libavutil/mem.c` | imported byte-identical | `cfb6d8ab8ffa6c6d51f325d4d8bbe7dc0922871f` |
| `libavutil/mem.h` | imported byte-identical | `e21a1feaaeab14d21ef93e3053e2de6b04b4097b` |
| `libavutil/mem_internal.h` | imported byte-identical | `ee2575c85f9cd04a1d9c4f483dfc02fb598f09f4` |
| `libavutil/opt.c` | imported byte-identical | `590146b5fbb9868af5bbef6a347e896f6221c9c6` |
| `libavutil/opt.h` | imported byte-identical | `8dc020a8200af547bb912f21555353580b0c3d20` |
| `libavutil/parseutils.c` | imported byte-identical | `7f678cd85af190535c2912910aeb06650a64161e` |
| `libavutil/parseutils.h` | imported byte-identical | `e66d24b76e5535579b87dd82e5768619158286ff` |
| `libavutil/pixdesc.c` | imported byte-identical | `18c7a0efc82104ff76e9da72a2a216108bfba6e3` |
| `libavutil/pixdesc.h` | imported byte-identical | `9b9d386ae96e86f8c7145c12763e89453202ad4a` |
| `libavutil/pixfmt.h` | imported byte-identical | `46ef211add11306f1d7144cc56f609a7e50d4c44` |
| `libavutil/qsort.h` | imported byte-identical | `39b7a088520a61a87c1565474cb4b32ea6ce766a` |
| `libavutil/random_seed.c` | imported byte-identical | `70dc509d2fde6ea0b6c30e971fbd8f1a13e15a19` |
| `libavutil/random_seed.h` | imported byte-identical | `0462a048e048317b0150aa3b800171a96a668e45` |
| `libavutil/rational.c` | imported byte-identical | `eb148ddb12cc3df93e969a681ece54a2780a7b19` |
| `libavutil/rational.h` | imported byte-identical | `cbb08a0baf16d28c924826bd7e2af9ed18a46866` |
| `libavutil/reverse.c` | imported byte-identical | `105eb03dda4c30952541347c6e6f7f2feb4f2f30` |
| `libavutil/reverse.h` | imported byte-identical | `4eb61239328120bb77ae3ebc34f4f911ad51936b` |
| `libavutil/samplefmt.c` | imported byte-identical | `fc077f644443634628fc6c461d844a21c9e72d1c` |
| `libavutil/samplefmt.h` | imported byte-identical | `8cd43ae8568a890f825299ee1e6d6f35ad1e5c5d` |
| `libavutil/sha.c` | imported byte-identical | `ef6fa4422740735606e88e9b61e98b49212e38ea` |
| `libavutil/sha.h` | imported byte-identical | `c0180e5729a2e9232bdbf387b9b74d6acc25e943` |
| `libavutil/stereo3d.c` | imported byte-identical | `279fcc1ff3e373c8821619ca8caad721e7c5c956` |
| `libavutil/stereo3d.h` | imported byte-identical | `d421aac2a26b12feb5ef94f5e5441832df556f48` |
| `libavutil/thread.h` | imported byte-identical | `be5c4b1340e7ad7b1bef227a44b78af44809c227` |
| `libavutil/time.c` | imported byte-identical | `740afc4785088b91b8de2e78d55baf813f1d4981` |
| `libavutil/time.h` | imported byte-identical | `dc169b064a0d7d4659809da42e0a6942449fba39` |
| `libavutil/time_internal.h` | imported byte-identical | `d0f007ab1c4f52a99e1f9e19c36afa8102923513` |
| `libavutil/timecode.c` | imported byte-identical | `2fc3295e25dedc328a2943673b7c8734570516c1` |
| `libavutil/timecode.h` | imported byte-identical | `060574a17203851aee45126f7ab1e0345e2f01fb` |
| `libavutil/timer.h` | imported byte-identical | `36f920e96aeddd5709df688133021453aa51d704` |
| `libavutil/utils.c` | imported byte-identical | `c1cd452eee51de5f81dcc0eb7f25f453cf82173b` |
| `libavutil/version.h` | imported byte-identical | `f888dbb2dc12e3f7180fe4ec639b453d924272e0` |
| `libavutil/video_enc_params.c` | imported byte-identical | `635176ab91d0f70b3e5550683d9ac594223caa54` |
| `libavutil/video_enc_params.h` | imported byte-identical | `fc0c3bc1a514f035bd5b8edec03e492bea93c007` |
| `LICENSE.LGPLv2.1.txt` | imported byte-identical (`COPYING.LGPLv2.1`) | `58af0d3787aec7d6f8d833535d64879156e7ec23` |

## Authored stand-ins

- `config.h`, `libavutil/avconfig.h`, `libavutil/ffversion.h`, `libavcodec/codec_list.c`,
  `libavcodec/bsf_list.c` replace what FFmpeg's `configure` generates. They select the generic C
  build: `ARCH_* 0`, `HAVE_INLINE_ASM 0`, `HAVE_THREADS 0`, every hwaccel `0`,
  `CONFIG_SAFE_BITSTREAM_READER 1` (the upstream default, the bounds-checked bit reader),
  `CONFIG_SMALL 0`, `CONFIG_ICONV 0`, byte order and word size from compiler macros, C99 math
  present, and the host allocator selected by `_WIN32` (`_aligned_malloc`) or POSIX
  (`posix_memalign`). `codec_list.c` registers only `ff_h264_decoder`; `bsf_list.c` only
  `ff_null_bsf`. Every `HAVE_*`/`ARCH_*`/`CONFIG_*` symbol the imported sources reference is
  listed, so a missing one is a compile error rather than a silent zero.
- `nk_h264_symbols.h` is force-included (`-include`) into every translation unit of this
  subset. The runtime also links the ATRAC3/ATRAC3+ subset, which carries its own copies of
  some libavutil/libavcodec files at the same pin, so the 38 global symbols the two subsets
  both define (the allocator, `av_log`, VLC and bit-copy helpers, `ff_log2_tab`, `ff_reverse`)
  are renamed with an `nkh264_` prefix here. This keeps the imported files byte-identical. A
  symbol missed by the list fails the runtime link with a duplicate-definition error.

## Build flags

Imported translation units are compiled by the Makefile pattern rule `h264sub_%.o` with
`-std=c11 -DHAVE_AV_CONFIG_H -D_ISOC99_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
-D_POSIX_C_SOURCE=200112 -D_XOPEN_SOURCE=600` (what FFmpeg's own build passes), `-w` (upstream
code is not held to the project warning set), the `nk_h264_symbols.h` force-include, and
`-I src/rt/h264_ffmpeg_sub`.

## Authored API layer

- `nk_h264_api.h` — the only header the runtime sees: opaque `NkH264Decoder`;
  `nk_h264_open(verbose)`, `nk_h264_send(data, size, pts, has_pts)`, `nk_h264_send_eos`,
  `nk_h264_receive(picture)`, `nk_h264_close`, `nk_h264_ident`. No FFmpeg type, macro or
  header leaks out. Results are `NK_H264_OK/AGAIN/EOF/INVALID_DATA/ERROR`; a picture is
  reported as planes, strides, size, one of YUV420P, NV12 or other, range and PTS.
- `nk_h264_api.c` — drives the unmodified decoder through `avcodec_send_packet` /
  `avcodec_receive_frame` with one thread (`thread_count = 1`), `AV_CODEC_FLAG_LOW_DELAY`,
  a 1/90000 packet time base, and `AV_NOPTS_VALUE` for access units without a source PTS.
- The seam backend that converts pictures into guest or host pixel buffers and keeps the
  fail-closed, reset and EOS contract is `src/rt/h264_ffmpeg.c` (project-authored).

## Licensing notes

- Imported FFmpeg files retain their original upstream SPDX/LGPL headers and copyright notices.
- This directory is LGPL-2.1-or-later (see `LICENSE.LGPLv2.1.txt`); the authored files are
  SPDX-tagged `LGPL-2.1-or-later` to be consistent with the imported core.
- Repository-level declaration remains GPL-3.0-or-later (LICENSE, NOTICE.md); per-file SPDX is
  preserved and deliberate. LGPL-2.1-or-later code is combinable with GPL-3.0-or-later.
- The build is an LGPL configuration (no `--enable-gpl`, no `--enable-nonfree`, no GPL-only
  component, `CONFIG_GPL 0`). The decoder is statically linked into the player, like the
  ATRAC3+ subset; NOTICE.md records the relinking and corresponding-source obligations.
- Several imported libavutil/libavcodec files are also in the ATRAC3+ subset; the
  `src/rt/atrac3p/` copies stay separate so each subset remains independently pinned.

## Verification

- 2026-10-24: all 208 imported blob SHAs match `git rev-parse n4.4:<path>` in a clone
  at the pin, and all 208 match the working-tree `git hash-object` (zero deltas).
- The subset compiles with GCC 13 (`-std=c11`) and the runtime test `make CC=gcc H264_FFMPEG=1
  psmf-media-selftest` decodes the source-authored H.264 fixture through it with no system
  FFmpeg installed.

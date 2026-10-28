# PROVENANCE.md — `third_party/libchdr/`

libchdr reads MAME's CHD (Compressed Hunks of Data) format. Nakagawa uses it to
expand a CHD disc image the user adds into a plain `.iso`, once, at import
(`src/core/nk_disc_chd.c`, the backend of `src/core/nk_disc_expand.c`). It is
compiled into the native player, the player UI test binary, the
`nk_disc_expand` command-line wrapper the Python tools build
(`tools/nk_core/disc_expand_tool.py`) and the `test_disc_expand` selftest. It is
not linked into the runtime, which never reads a CHD.

## Upstream source

- Project: libchdr
- Repository: `https://github.com/rtissera/libchdr`
- Pin: commit `47d794151b2b64f6aba60304e6480f4f5a2bc24b` (2026-10-11, "Merge pull
  request #198 from 0x000NULL/fix-crafted-chd"), the head of the default branch
  `master` at the time of import. Upstream has no release tags.
- License: BSD-3-Clause, `Copyright Romain Tisserand` (`LICENSE.txt`). libchdr
  carries three third-party codecs and a FLAC reader, each under its own terms
  (below); all are compatible with GPL-3.0-or-later, so they are imported under
  `AGENTS.md` section 4's import rules.
- Import method: every libchdr file was written from its Git blob at the pinned
  commit (`git cat-file blob <commit>:<path>`), so the bytes are the upstream
  bytes, including the CRLF line endings some of them use. `.gitattributes`
  turns end-of-line conversion and whitespace checks off for this folder so the
  stored bytes stay identical. The Git blob IDs below match the files at the
  pin; the SHA-256 values were calculated from the local bytes.

### Codecs libchdr carries

| Folder | Component | Upstream version | License | Copyright |
| --- | --- | --- | --- | --- |
| `deps/miniz-3.1.2/` | miniz (inflate only) | 3.1.2 | MIT | 2013-2014 RAD Game Tools and Valve Software; 2010-2014 Rich Geldreich and Tenacious Software LLC |
| `deps/zstd-1.5.7/` | Zstandard single-file decompressor | 1.5.7 | BSD-3-Clause (upstream also offers GPL-2.0-only; the project uses BSD-3-Clause) | Meta Platforms, Inc. and affiliates |
| `deps/lzma-26.02/` | LZMA SDK decoder | 26.02 (`LzmaDec.c` dated 2023-04-07) | public domain (SPDX `LZMA-SDK-9.22` text) | Igor Pavlov |
| `include/dr_libs/` | dr_flac | v0.13.4 | Unlicense or MIT-0; the project elects the Unlicense, as for `third_party/dr_libs/` | David Reid |

The codec sources are the copies in libchdr's `deps/` and `include/dr_libs/`
at the pin. libchdr does not carry the miniz and Zstandard licence files, so
each was imported byte-identical from its own upstream at the tag libchdr's
folder names:

- `deps/miniz-3.1.2/LICENSE`: `https://github.com/richgel999/miniz`, tag `3.1.2`,
  commit `77d0dce8627735138c51770d1799a1ef48f2117d`, file `LICENSE`.
- `deps/zstd-1.5.7/LICENSE`: `https://github.com/facebook/zstd`, tag `v1.5.7`,
  commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99`, file `LICENSE`.

Both were downloaded from `raw.githubusercontent.com` at those commits; their
Git blob IDs match the files the GitHub contents API reports at those commits.

## What is built, and how

Upstream's default CMake configuration, written out in the `Makefile`
(`LIBCHDR_SRCS`, `LIBCHDR_CFLAGS`), in `CMakeLists.txt` (`nk_libchdr`) and in
`tools/nk_core/disc_expand_tool.py`: the bundled miniz (`MINIZ_NO_ARCHIVE_APIS`,
`MINIZ_NO_DEFLATE_APIS`, `MINIZ_NO_STDIO`, `MINIZ_NO_TIME`, as upstream's
`deps/miniz-3.1.2/CMakeLists.txt` sets them), the bundled single-file Zstandard
decompressor, the LZMA SDK decoder in C (no assembly), the dr_flac FLAC
backend, and `chdconfig.h`'s defaults (raw data sectors, subcode and per-hunk
CRC checks on; no low-RAM mode). Upstream code is compiled without the
project's warning flags. Upstream's CMake files, the LZMA assembly, the
micro-flac backend (`src/libchdr_flac_microflac.cpp`), the linker script
(`src/link.T`), the tests and the README were not imported. No system zlib or
zstd is linked, so the player's runtime DLL set is unchanged.

## File manifest

| File | Kind | Upstream blob | SHA-256 |
| --- | --- | --- | --- |
| `LICENSE.txt` | imported byte-identical | `1c36e5b593498866a8ea21aa968c180acac3b967` | `e3989a7aee74a91b8cdbdfe204a6b8d03288863ec7533a21c51f1a987ba73539` |
| `include/libchdr/bitstream.h` | imported byte-identical | `60f20649739c7f66a43019a40efaaede252da2d7` | `fc48799ca3df009a9e114772e69be8f2bea634e4e3c64e128bda6cfbf314020f` |
| `include/libchdr/cdrom.h` | imported byte-identical | `680aa39576f8960cd93ad58f82c78e696fcbf670` | `eea4668fdb548cbd4cd6ac033441c7725eb0623a8cd0ea966a064f3b5328495b` |
| `include/libchdr/chd.h` | imported byte-identical | `f07c374e8708ffa388c6519fa79b8613c5fd819a` | `a318ad8a93050bbe33fdc18dcddc1b433d1840d2f09e42166e090ad5838f5fe4` |
| `include/libchdr/chdconfig.h` | imported byte-identical | `eac64917d39fbfaa5dfcc150e5d4d6b2284ccbd0` | `b0397b337d9631d1f59c401bab29ddda3dbe9154721efdc3627a8a082136cb51` |
| `include/libchdr/coretypes.h` | imported byte-identical | `11692d70b9cd9fd924a4fb9680598c33951c5d38` | `f573f4ddf8960bff863d1e8f3513acad71a22807a18eccb634a4a4519cb88d72` |
| `include/libchdr/flac.h` | imported byte-identical | `49a3ba90bf44ee3907afe4e9d69cf755656f1757` | `e333975b5ce21afa3d918e769b264df20b18eb7ed8e0388a7e1920790ebd9fc2` |
| `include/libchdr/huffman.h` | imported byte-identical | `8bfacd05601144dacbf80b9aa9a24f07f2bc08aa` | `6794c036b503fec9b97d1defd7f284b8e54aec94e6ab1d7a432a2ee84c5013c4` |
| `include/libchdr/macros.h` | imported byte-identical | `445b3b247af4e719026e84cdb664cc88d0b5f068` | `99724cf310cb46c2fbfb558ed1005397a6c4b43864f11ace628b0f7bc8cd72e6` |
| `include/dr_libs/dr_flac.h` | imported byte-identical | `051f37c66e5394203efa80e7d048579b4540c078` | `111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c` |
| `src/codec_avhuff.h` | imported byte-identical | `774d1f290619f32f1315359283588e7c6dac3889` | `d13a9d4cb334e3667de8e1f1f057080d9df27a18f3f8208b3b8a8cc047c9ad5d` |
| `src/codec_cdfl.h` | imported byte-identical | `a495bf673d9c1c7e055a4fa7476f4950f8469c93` | `18974d70e7c8703846034ca7a73fd3fc041dbea56c9caa3fc22caa8bab75e0ab` |
| `src/codec_cdlz.h` | imported byte-identical | `2b6796823e4b10e492c9b5fc2a4e7c1d3fcb28e0` | `936fed6ad6b71228e719d5226ae1cad85486830fcc40da9e3eaba6dcaca6a5c7` |
| `src/codec_cdzl.h` | imported byte-identical | `6db871840a802daaea17df61603710ea271132f8` | `b18491b60e7130bb8d5253bdebb6d5b4e1930853b5785849bce9b0d7321a94b5` |
| `src/codec_cdzs.h` | imported byte-identical | `a34d33d9f0a4e503b63141e63f7a06e9bd534184` | `b3138357a7463bd044d72f568a891ffbc0802f502a5f2e5440a52f07da167687` |
| `src/codec_flac.h` | imported byte-identical | `93c2af1f2de4a971dcb1f274a5d35623eee71aee` | `6e431bfee6d6604192a99a9ccdcf9040e9d56e218bbb23086bc5e42024a5eb0f` |
| `src/codec_huff.h` | imported byte-identical | `dcd669cf4b5b66b5af72bf2d5c04dd095c36c605` | `0ceedce46afe7b581f0d9779d511653dd435f4601d428b6fdd8daf7e6f3789ed` |
| `src/codec_lzma.h` | imported byte-identical | `821862e6644742c541d30014f95da38da283efdf` | `97f1f51c9ce547ec85d8a0c7aff3024b635e35ce37e347172cdae8ea0056f5c7` |
| `src/codec_zlib.h` | imported byte-identical | `f36b430a5d4b273d7ac4b840c9180368b0de4bc6` | `eef6562dc312a0eda5ca41f9ad1d035cb6aafbb065bbeafbd655d115ec83c640` |
| `src/codec_zstd.h` | imported byte-identical | `a90e52cc06aedcad7b33c479ac04d0f3e667e830` | `c10f7be9073c790fa9f9f4cb8404f918487fe91ad40ecfc4c8e2acb19b347c62` |
| `src/libchdr_bitstream.c` | imported byte-identical | `4fb1c63b2f67f4d925e2c662acdd2c44493f92dd` | `02ec6d7c55e41b1ebfea5874d1aec2ffaabc086df102e19154e7e023a6bd0a76` |
| `src/libchdr_cdrom.c` | imported byte-identical | `2282aa292d8aa40580f16384f8b53d5fafb026df` | `7fc4515eb3598d88797b2786a560bd497720eb82f9b8c6391ba4ab7a74f5e351` |
| `src/libchdr_chd.c` | imported byte-identical | `d69fbed34ecd89ced9aec8692e4460f197fedc0b` | `8c1cfd8f26c3cded7b727b59059953c55b97b2c4d3fca129e19dd4157f450b14` |
| `src/libchdr_codec_avhuff.c` | imported byte-identical | `5f3a44321cc4b92e9928bb37660c4c1ed691a598` | `9504a303dd4cfc26befe866861d2bd2b7dbaf35b67185f73a6f81fbb5b13148c` |
| `src/libchdr_codec_cdfl.c` | imported byte-identical | `fe98d92b4214c0766755feacb44f8ad811349632` | `11e9b5e9a6982024a892130fde58f243936719162f951d33b48aa3b2b29ff78a` |
| `src/libchdr_codec_cdlz.c` | imported byte-identical | `4018a89d28361a6e217f8746e5304a76719797a9` | `8e62e01993aec7e27e847f1008ea6dec476de25b4800083abf6502d8c6952393` |
| `src/libchdr_codec_cdzl.c` | imported byte-identical | `8fdb721841155a2e794343cf16f7aededec181eb` | `e14acfe1c44cb69bd24b052e386b81b36fc7f5f7f1a062b9a1e41de0088f985d` |
| `src/libchdr_codec_cdzs.c` | imported byte-identical | `6679202e03eefef5068a8fa29d7b2f3feec8fd4c` | `4fbe14e3727fcd6ed592be406ca1ea6d9c074bd3f6acca57100af07dcaf1e301` |
| `src/libchdr_codec_flac.c` | imported byte-identical | `32aa7221debd5e07536017c40cb273db231104ce` | `feafae3d2e5c342f35ae1c3d941028c7acc594c319f53d225d1c56592fc78697` |
| `src/libchdr_codec_huff.c` | imported byte-identical | `6453d6857d7b5773c533bec486d45b14d39a4f6f` | `b8ea9c56c436a4dfa9e97a8f78e0caad3ec490a375acb0ecc617d22cdbd91f73` |
| `src/libchdr_codec_lzma.c` | imported byte-identical | `bd308541f05193d687853cd2f9dfff187876234e` | `912932f585dc12b8f399b49b718a3c9533049efa2393b42d51240a22813d8fcd` |
| `src/libchdr_codec_zlib.c` | imported byte-identical | `6d299fe369a2a45495976e7537d77f3c60803bfd` | `f15fee532e749dfd6933291e2fb36736b62c71a8986b2b7afdb3bfd6bc0d1961` |
| `src/libchdr_codec_zstd.c` | imported byte-identical | `5390d5d415aaadaf78a5a630b81c90e00af2464c` | `92efc390845a76d027cf2c3bbaae0c7643109f3b3654fea07ed01871a9a3b76a` |
| `src/libchdr_flac.c` | imported byte-identical | `2d1ad10e255deaa65a6debad641afb01b203756e` | `ee8dc53130ee5a86df4a783e6f55cdb5cba6856e340c7e7feff40ff314461cc7` |
| `src/libchdr_huffman.c` | imported byte-identical | `16d96b5f627f6a69dd407fc1f17b51cd232a91d9` | `1d9617d27ec324674157dbe3e8284ed7ea32d6c6cf5b802996086559cef249f2` |
| `deps/miniz-3.1.2/miniz.c` | imported byte-identical | `54fc5f39ef444e7cfd768ca8e46cee12684dc4c1` | `e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30` |
| `deps/miniz-3.1.2/miniz.h` | imported byte-identical | `d8244d085b6d2353e7056045c5aff4bd11c143c6` | `b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f` |
| `deps/miniz-3.1.2/LICENSE` | imported byte-identical (miniz `3.1.2`) | `1982f4bb8f4afa3dcbe803887fb4af7573430e51` | `0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3` |
| `deps/zstd-1.5.7/zstd.h` | imported byte-identical | `b8c0644a7ec6533916e06aa8392a2727cbe55573` | `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b` |
| `deps/zstd-1.5.7/zstd_errors.h` | imported byte-identical | `8ebc95cbb2ab882027a3e6cc92cdd5abc1a0dee4` | `66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a` |
| `deps/zstd-1.5.7/zstddeclib.c` | imported byte-identical | `a7623f8a417a55dcd08882a772358b94b55e7caa` | `e1dc239cb4bcf3c00a0462fdbe5f3e3c78f907c4f8cb02c048807d754bdc51b5` |
| `deps/zstd-1.5.7/LICENSE` | imported byte-identical (Zstandard `v1.5.7`) | `75800288cc243f164b9d130f125e2ffae28f8e39` | `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8` |
| `deps/lzma-26.02/LICENSE` | imported byte-identical | `5f57051693886e063b523267829fee2abbd419ce` | `06721414a6decad6d6677feb470f01f47126e154d895d85bc1e17628697715e5` |
| `deps/lzma-26.02/include/LzmaDec.h` | imported byte-identical | `0aeba2d5f827087c6e118ed72c074c4ab1321588` | `fa85869a8d2fff5ceccff456b6e980204cadc97430aa2142adab4a26be549647` |
| `deps/lzma-26.02/include/real/7zTypes.h` | imported byte-identical | `8aaabc8fbca8b49ba73ede50b65f3a7d97d829da` | `b166eeaf8e4cab98c1403177736a127c55c5665c4be8897b777d237cb3e36763` |
| `deps/lzma-26.02/include/real/LzmaDec.h` | imported byte-identical | `b0ce28fa02a1adc422b7269c4e6d2c2d26fc60b9` | `3aaf07b4ae4173a2d103179455dc7089b5ddbc7fc3db3c0e40964a7499c69266` |
| `deps/lzma-26.02/src/LzmaDec.c` | imported byte-identical | `4772470af2daa31947894f73ea25fedfd25dd8ac` | `63f5f06b029623f8ac9f691a04297a2b2a3d19c880c091719e4db2f20bea2662` |
| `deps/lzma-26.02/src/real/LzmaDec.c` | imported byte-identical | `ceeec5196af233511e50f409064f748ffdc65d4b` | `e9eaa3b0106c4c08da8080e40c9e32cf20c4bb6f1ec34ea394becec325d71d11` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Local deltas

None. Every imported file is byte-identical to its upstream blob.

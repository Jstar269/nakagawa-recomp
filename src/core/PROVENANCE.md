# PROVENANCE.md — `src/core/nk_psp_*` (built-in PSP game decryption)

The decryption boundary that turns the encrypted executable and modules of a game the user owns into plain ELF files,
locally, from the user's own disc image (issue #295). It was added in PRs #548 and #550, removed as a temporary measure
in PR #1187, and restored with built-in key tables after the maintainer's decision of 2026-10-27 to decrypt the way
PPSSPP does. Firmware is never decrypted: updater PSAR archives, the IPL, the Media Engine image and system-menu files are
refused by name before any decryption step runs (`AGENTS.md` section 5).

Every derived file below is labelled `derived-translated` in the trusted provenance record
(`docs/provenance/INDEPENDENCE_MODEL.md`); none is "independent". The project-authored files are listed last.

## Built-in key tables (`LOCAL DELTA`)

- Files: `nk_psp_builtin_keys.c`, `nk_psp_builtin_keys.h`.
- Upstream: PPSSPP, `https://github.com/hrydgard/ppsspp`, revision `b4386554795949491a26e51b7d74082bb9b2470f`.
  - `Core/ELF/PrxDecrypter.cpp` (SHA-256 `3ab45acda6b80752ce0c5aaef1abc3a80903beb2d9dfc59b477cfe8793660133`),
    Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later: the tag key arrays and the `g_tagInfo`/`g_tagInfo2` recipes.
  - `ext/libkirk/kirk_engine.c` (SHA-256 `cfd70093c3e6572b9862dc61cd99ebea117eb92feb3e19a9cd393f7a1af0598c`),
    KIRK-ENGINE by Draan and contributors, GPL-3.0-or-later: `kirk1_key`, the curve-1 ECDSA parameters and public
    point, and the keyvault rows the recipes use.
- Licence: the file is GPL-3.0-or-later, which both upstream licences permit; both copyright notices are kept in its
  header.
- LOCAL DELTA: the values and array names are unchanged. The file is C data in one table; a tag that appears in both
  upstream tables (a 144-byte and a 16-byte recipe with the same code) is one entry; the 144-byte keys keep upstream's
  32-bit words and the KeyStore serialises them little-endian, as upstream's byte view does on the PSP. Left out:
  - every firmware container tag: `0x0B000000` (updater executable), `0x0E000000` (updater DATA.PSAR block),
    `0x0F000000` and `0x862648D1` (Media Engine image), `0x07000000` and the seventeen `sceResmgr_9DC14891` index.dat
    tags (`0x0B2B05F0` to `0x0B2B92F0`, `0x495BE403`). They are listed in `nk_psp_firmware_tags` only, so the boundary
    can refuse them by name; their keys are not carried;
  - the type-3 DRM BB patch tag `0x89742B04`, the SPRX decrypt-mode pad `xor_91E0A9AD` and the KIRK command-2 results,
    which need the installing console's context (PBP/NPDRM, a later phase);
  - every KIRK value no game recipe uses: curve 2, the signing key `kirk16_key`, and the keyvault rows no included
    recipe names (among them `0x4E`, the updater's, and `0x52`, the Media Engine image's).
- Regenerating: the table is a mechanical extraction of the two pinned files under the rules above. A refresh pins a new
  revision here, records both SHA-256 values, and keeps the exclusions.

## Container and KIRK engine (`LOCAL DELTA`, restored from PR #548)

- Upstream: John-K/pspdecrypt, `https://github.com/John-K/pspdecrypt`, revision
  `c156627db7634d395c380c0a9589130f603307fc`, `LICENSE.TXT` GNU GPL version 3 (no later-version grant). Its
  `PrxDecrypter.cpp` descends from PPSSPP's (GPL-2.0-or-later).
- Files and their upstream paths:
  - `nk_psp_kirk.c/.h` from `libkirk/kirk_engine.c/.h` (KIRK-ENGINE, GPL-3.0-or-later per its own notice);
  - `nk_psp_aes.c/.h` from `libkirk/AES.c/.h` (public-domain Rijndael notice kept), GPL-3.0-only;
  - `nk_psp_sha1.c/.h` from `libkirk/SHA1.c/.h` (public-domain SHA-1 notice kept), GPL-3.0-only;
  - `nk_psp_kle.c/.h` from `kl4e.c/.h`, GPL-3.0-only;
  - `nk_psp_prx.c/.h` from `PrxDecrypter.cpp/.h`, GPL-3.0-only.
- LOCAL DELTA: portable C99; every embedded key, tag table and PSAR constant removed (recipes come from the KeyStore,
  which the built-in tables above fill); KIRK operations thread a context so a missing entry fails closed by name. Each
  file header lists its own changes. The 2026-10-27 restoration changed only `nk_psp_prx.c`'s missing-recipe message.
- Licence note: a GPL-3.0-only file can be combined with the project's GPL-3.0-or-later files; the player and
  `nk_decrypt`, which link them, are conveyed under the GNU GPL version 3 (`NOTICE.md`).

## Project-authored files

`nk_psp_container.c/.h` (framing, decompression dispatch, the firmware refusal), `nk_psp_keystore.c/.h` (built-in and
key-file loading, no PSAR entry name), `nk_psp_crypto.c/.h` (the error contract and result names), `nk_psp_ec.c/.h`,
`nk_psp_inflate.c/.h` and `nk_decrypt_main.c` (the `nk_decrypt` command line) are project-authored from the format rules
and the standards their headers cite (FIPS 197, RFC 4493, FIPS 180, RFC 1950-1952, SEC 1).

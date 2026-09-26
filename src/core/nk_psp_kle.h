/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Ported from kl4e.h of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc.  Upstream carries no
 * per-file licence header; the repository's LICENSE.TXT is the GNU GPL
 * version 3 (the Readme states "Licensed under GPLv3"), so this port is
 * labelled GPL-3.0-only.  Original author artart78; his notice is kept
 * verbatim in nk_psp_kle.c.
 *
 * Modified for this project: header guard, SPDX/attribution, and an
 * include of the shared crypto contract for the u8 type alias.
 */

#ifndef NK_PSP_KLE_H
#define NK_PSP_KLE_H

#include "nk_psp_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decompress a KL4E/KL3E stream.  inBuf points PAST the 4-byte magic
 * ("KL4E"/"KL3E"); isKl4e selects the variant.  On success returns 0 and
 * sets *end to the end of the consumed input; returns non-zero on a
 * malformed stream (fail closed).
 */
int decompress_kle(u8 *outBuf, int outSize, u8 *inBuf, void **end, int isKl4e);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_KLE_H */

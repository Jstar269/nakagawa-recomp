/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors
 *
 * PSP PGD (amctrl) decryption for the runtime. Self-contained: depends only on
 * <stdio.h>/<stdint.h>, no CpuState/guest-memory glue, so it builds and tests
 * standalone. This is a C port of tools/pgd_decrypt.py. The PSP-specific
 * BBMac/BBCipher/PGD flow is derived-translated; AES-128 and later defensive
 * runtime hardening are independently expressed. See
 * docs/provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md. The fixed-key
 * path uses locally supplied PSP platform data and no per-device fuse key.
 */
#ifndef SR_PGD_H
#define SR_PGD_H

#include "pgd_api.h"

/* Maximum accepted PGD block_size (bytes). The format itself publishes no
 * limit: the reference pgd_open (PPSSPP ext/libkirk/amctrl.c, tpunix's
 * kirk_engine) reads block_size from the decrypted header and mallocs
 * block_size*2 with no validation, and NPDRM authoring tools emit fixed
 * small blocks (0x400 in common PGD tooling, 0x800 in sign_np's
 * encrypt_pgd). Since no authoritative bound exists, this is a documented
 * allocation-safety/compatibility cap -- a project policy, not a PSP
 * hardware or firmware specification: 1 MiB is >= 512x every observed
 * real-world value while limiting what an untrusted header can make
 * sr_pgd_open allocate to 2 MiB total. It may be raised in a reviewed
 * change if a legitimate file requires it; the point is that the bound is
 * a reviewed constant, not attacker input. */
#define SR_PGD_MAX_BLOCK_SIZE 0x100000u

/* Verify AES-128 against the NIST FIPS-197 known-answer vector. Returns 1 on
 * success. Cheap; call once before trusting decryption. Does not require the
 * locally supplied console constants. */

/* The PSP KIRK/amctrl constants this path needs are console decryption values
 * and are deliberately not shipped with this project. They are read once from
 * $SR_PGD_KEYS, else ./keys/pgd_keys.txt; see docs/PGD_KEYS.md for the schema.
 * Returns 1 when the complete set is installed. sr_pgd_open returns NULL when
 * it is not, which is indistinguishable from a wrong key by design -- call this
 * first if you want to tell the user which of the two happened. */

/* Path the constants are read from, for diagnostics. Never returns NULL. */

/* Open a PGD context from the 0x90-byte header and the 16-byte version key.
 * Returns NULL unless BOTH header MACs verify (wrong key, non-fixed-key DRM
 * type, or corrupt header) -- so a non-NULL result proves the decryption is
 * correct. Free with sr_pgd_free. */

/* Return a pointer to decrypted block `index` (block_size bytes, last block may
 * be shorter -- see sr_pgd_block_len), reading ciphertext from `host` at
 * data_offset + index*block_size. The pointer is owned by `p` and valid until
 * the next call or sr_pgd_free. Returns NULL on a host read error. One block is
 * cached, so repeated/sequential reads of the same block do not re-decrypt. */
#endif /* SR_PGD_H */

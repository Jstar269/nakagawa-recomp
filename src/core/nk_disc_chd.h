/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The libchdr-backed CHD reader for nk_disc_expand.c. Only programs that link
 * the vendored libchdr (the player and the nk_disc_expand wrapper) include
 * this; the others refuse CHD by name. */

#ifndef NK_DISC_CHD_H
#define NK_DISC_CHD_H

#include "nk_disc_expand.h"

#ifdef __cplusplus
extern "C" {
#endif

const NkDiscChdBackend *nk_disc_chd_libchdr_backend(void);
/* nk_disc_set_chd_backend(nk_disc_chd_libchdr_backend()). */
void nk_disc_chd_register(void);

#ifdef __cplusplus
}
#endif

#endif /* NK_DISC_CHD_H */

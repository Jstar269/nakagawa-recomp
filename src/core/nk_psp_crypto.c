/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Fail-closed context helpers for the PSP decryption boundary (issue #295):
 * KeyStore lookups record the exact missing entry name so callers can say
 * "this executable needs key entry <name>" instead of failing generically.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nk_psp_crypto.h"
#include "nk_psp_keystore.h"

void nk_psp_ctx_init(NkPspCtx *ctx, const NkKeystore *ks)
{
    if (ctx == NULL) return;
    ctx->ks = ks;
    ctx->missing_entry[0] = '\0';
    ctx->message[0] = '\0';
}

int nk_psp_need(NkPspCtx *ctx, const char *name,
                const uint8_t **out, size_t *out_len)
{
    if (ctx == NULL) return NK_PSP_ERR_INTERNAL;
    if (ctx->ks != NULL &&
        nk_keystore_get(ctx->ks, name, out, out_len) == 0) {
        return NK_PSP_OK;
    }
    if (ctx->missing_entry[0] == '\0') {
        snprintf(ctx->missing_entry, sizeof(ctx->missing_entry), "%s", name);
        snprintf(ctx->message, sizeof(ctx->message),
                 "this executable needs key entry %s; add it to the local key "
                 "file the boundary was given (see docs/SETUP.md)", name);
    }
    return NK_PSP_ERR_MISSING_KEY;
}

int nk_psp_fail(NkPspCtx *ctx, int code, const char *entry, const char *fmt, ...)
{
    if (ctx != NULL) {
        if (entry != NULL && ctx->missing_entry[0] == '\0') {
            snprintf(ctx->missing_entry, sizeof(ctx->missing_entry), "%s", entry);
        }
        va_list args;
        va_start(args, fmt);
        vsnprintf(ctx->message, sizeof(ctx->message), fmt, args);
        va_end(args);
    }
    return code;
}

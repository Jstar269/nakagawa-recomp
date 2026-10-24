/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_sig.h"

#include "monocypher-ed25519.h"

bool nk_ed25519_verify(const uint8_t sig[NK_ED25519_SIGNATURE_BYTES],
                       const uint8_t pub[NK_ED25519_PUBLIC_KEY_BYTES],
                       const uint8_t *msg, size_t len) {
    static const uint8_t empty[1] = {0};
    if (sig == NULL || pub == NULL || (msg == NULL && len != 0u)) {
        return false;
    }
    if (msg == NULL) {
        msg = empty;
    }
    /* crypto_ed25519_check returns 0 for a valid signature and -1 otherwise. */
    return crypto_ed25519_check(sig, pub, msg, len) == 0;
}

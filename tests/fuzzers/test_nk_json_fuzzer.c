/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_json.h"

#include <stddef.h>
#include <stdint.h>

#define NK_JSON_FUZZ_MAX_BYTES ((size_t)4096u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > NK_JSON_FUZZ_MAX_BYTES) return 0;

    char error[128];
    NkJsonNode *root = nk_json_parse((const char *)data, size, error, sizeof(error));
    nk_json_free(root);
    return 0;
}

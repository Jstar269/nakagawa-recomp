/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The vendored portable-snippets safe-math header, included the way
   src/core/nk_xb.c and src/core/nk_iso.c include it, at the edges those call
   sites rely on: the last representable size_t product and uint64_t sum
   succeed and one past them is refused. Building this file with -Wpedantic
   -Werror proves the quiet include compiles on each host. */

#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "../../third_party/portable-snippets/safe-math/safe-math.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int g_failures = 0;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);       \
        }                                                                  \
    } while (0)

static void test_size_mul(void) {
    size_t r = 0;
    const size_t half = SIZE_MAX / 2u;
    CHECK(psnip_safe_size_mul(&r, 0u, SIZE_MAX) && r == 0u, "0 * max");
    CHECK(psnip_safe_size_mul(&r, 3u, 8u) && r == 24u, "3 * 8");
    CHECK(psnip_safe_size_mul(&r, half, 2u) && r == SIZE_MAX - 1u, "half * 2 fits");
    CHECK(!psnip_safe_size_mul(&r, half + 1u, 2u), "(half + 1) * 2 refused");
    CHECK(!psnip_safe_size_mul(&r, SIZE_MAX, SIZE_MAX), "max * max refused");
    CHECK(!psnip_safe_size_mul(&r, SIZE_MAX / 8u + 1u, 8u), "one 8-byte row past max refused");
    /* The XB filesystem table: a 32-bit file count times its 8-byte row fits
       exactly when size_t is 64-bit. */
    CHECK(psnip_safe_size_mul(&r, (size_t)UINT32_MAX, 8u) == (sizeof(size_t) >= 8u),
          "u32 count * 8");
}

static void test_u64_add(void) {
    uint64_t r = 0;
    CHECK(psnip_safe_uint64_add(&r, UINT64_MAX - 1u, 1u) && r == UINT64_MAX, "(max - 1) + 1");
    CHECK(!psnip_safe_uint64_add(&r, UINT64_MAX, 1u), "max + 1 refused");
    CHECK(!psnip_safe_uint64_add(&r, 1u, UINT64_MAX), "1 + max refused");
    /* An ELF table end in nk_iso.c: a 32-bit offset plus a 16-bit entry size
       times a 16-bit count. */
    CHECK(psnip_safe_uint64_add(&r, UINT32_MAX, (uint64_t)UINT16_MAX * UINT16_MAX) &&
              r == (uint64_t)UINT32_MAX + (uint64_t)UINT16_MAX * UINT16_MAX,
          "ELF table end");
}

int main(void) {
    test_size_mul();
    test_u64_add();
    if (g_failures != 0) {
        fprintf(stderr, "test_safe_math: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_safe_math: OK\n");
    return 0;
}

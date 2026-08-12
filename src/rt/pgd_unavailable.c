/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors */

/* Fail-closed backend for source distributions that exclude PGD/amctrl. */

#include "pgd_api.h"

struct SrPgd { unsigned unused; };

int sr_pgd_selftest(void) { return 0; }
int sr_pgd_keys_available(void) { return 0; }
const char *sr_pgd_keys_path(void) { return "PGD backend excluded"; }
SrPgd *sr_pgd_open(const uint8_t header[0x90], const uint8_t vkey[16]) {
    (void)header; (void)vkey; return NULL;
}
uint32_t sr_pgd_data_size(const SrPgd *p) { (void)p; return 0; }
uint32_t sr_pgd_block_size(const SrPgd *p) { (void)p; return 0; }
uint32_t sr_pgd_data_offset(const SrPgd *p) { (void)p; return 0; }
const uint8_t *sr_pgd_block(SrPgd *p, FILE *host, uint32_t index) {
    (void)p; (void)host; (void)index; return NULL;
}
uint32_t sr_pgd_block_len(const SrPgd *p, uint32_t index) { (void)p; (void)index; return 0; }
void sr_pgd_free(SrPgd *p) { (void)p; }

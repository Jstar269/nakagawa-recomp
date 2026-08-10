/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors */

#ifndef SR_PGD_API_H
#define SR_PGD_API_H

#include <stdint.h>
#include <stdio.h>

typedef struct SrPgd SrPgd;

int sr_pgd_selftest(void);
int sr_pgd_keys_available(void);
const char *sr_pgd_keys_path(void);
SrPgd *sr_pgd_open(const uint8_t header[0x90], const uint8_t vkey[16]);
uint32_t sr_pgd_data_size(const SrPgd *p);
uint32_t sr_pgd_block_size(const SrPgd *p);
uint32_t sr_pgd_data_offset(const SrPgd *p);
const uint8_t *sr_pgd_block(SrPgd *p, FILE *host, uint32_t index);
uint32_t sr_pgd_block_len(const SrPgd *p, uint32_t index);
void sr_pgd_free(SrPgd *p);

#endif

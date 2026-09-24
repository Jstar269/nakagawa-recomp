// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#ifndef COSIM_FPU_REFERENCE_H
#define COSIM_FPU_REFERENCE_H

#include <stdint.h>

enum FpuReferenceStatus {
    COSIM_FPU_REFERENCE_OK = 0,
    COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN = 1,
    COSIM_FPU_REFERENCE_INVALID = 2,
};

enum FpuReferenceBinaryOp {
    COSIM_FPU_REFERENCE_ADD = 0,
    COSIM_FPU_REFERENCE_SUB = 1,
    COSIM_FPU_REFERENCE_MUL = 2,
    COSIM_FPU_REFERENCE_DIV = 3,
};

enum FpuReferenceUnaryOp {
    COSIM_FPU_REFERENCE_SQRT = 4,
    COSIM_FPU_REFERENCE_ABS = 5,
    COSIM_FPU_REFERENCE_MOV = 6,
    COSIM_FPU_REFERENCE_NEG = 7,
};

uint32_t fpu_reference_binary(unsigned op, uint32_t a_bits, uint32_t b_bits,
                              uint32_t fcr31,
                              enum FpuReferenceStatus *status);
uint32_t fpu_reference_unary(unsigned op, uint32_t a_bits, uint32_t fcr31,
                             enum FpuReferenceStatus *status);
uint32_t fpu_reference_to_word(uint32_t bits, unsigned funct, uint32_t fcr31,
                               enum FpuReferenceStatus *status);
uint32_t fpu_reference_cvt_s_w(int32_t value, uint32_t fcr31,
                               enum FpuReferenceStatus *status);
unsigned fpu_reference_compare(unsigned condition, uint32_t a_bits,
                               uint32_t b_bits,
                               enum FpuReferenceStatus *status);
unsigned fpu_reference_bc1(unsigned tf, unsigned condition);

#endif

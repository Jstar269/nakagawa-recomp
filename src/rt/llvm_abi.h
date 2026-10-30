// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// The runtime entry points the LLVM backend's generated IR calls (#330,
// docs/LLVM_BACKEND.md). Generated C reaches these helpers through macros and
// static inline functions in recomp.h; LLVM IR cannot expand a C macro, so each
// one gets a named entry point here whose body is exactly that macro or inline
// helper. Nothing in this file has semantics of its own.
//
// Delivery compiles src/rt/llvm_abi.c to bitcode with the generated-code flags
// (TRACE, PERF_AOT_INSTRUCTIONS, the stack census) and links the bitcode into each
// generated module before code generation (llvm-link --only-needed --internalize),
// so the IR inlines the same fast paths the C chunks do and a release build keeps
// the trace hooks compiled out.
//
// tools/codegen_llvm.py declares these with the same types (ABI_FUNCTIONS);
// tools/test_codegen_llvm.py compiles this header with Clang and checks that every
// declaration there matches the one the backend prints.

#ifndef SR_LLVM_ABI_H
#define SR_LLVM_ABI_H

#include "recomp.h"

/* sr_begin / sr_end: the per-instruction trace hooks. */
void sr_llvm_begin(CpuState *s, uint32_t pc, uint32_t op);
void sr_llvm_end(CpuState *s, uint32_t mem_addr, int32_t mem_size);

/* SR_YIELD: the preemption point at entry and on every backward branch. */
void sr_llvm_yield(CpuState *s, uint32_t target_pc);

/* SR_HOST_STACK_CHECK for a body that makes a direct CALL; `frame` is the body's
 * own frame address. */
void sr_llvm_host_stack_check(uint32_t entry, void *frame);

/* MEM_R8/MEM_R16/MEM_R32 (zero-extended to 32 bits) and MEM_W*_PC (the value is
 * truncated to the access width, as the macros cast it). */
uint32_t sr_llvm_r8(uint32_t addr);
uint32_t sr_llvm_r16(uint32_t addr);
uint32_t sr_llvm_r32(uint32_t addr);
void sr_llvm_w8_pc(uint32_t addr, uint32_t value, uint32_t pc);
void sr_llvm_w16_pc(uint32_t addr, uint32_t value, uint32_t pc);
void sr_llvm_w32_pc(uint32_t addr, uint32_t value, uint32_t pc);

/* SR_AOT_RETURN_CAPTURE: a `jr $ra` latches its target before the delay slot. */
void sr_llvm_return_capture(CpuState *s, uint32_t target);

/* sr_tail_record and SR_TAIL_DRAIN: the TAIL transfer protocol (#855). */
void sr_llvm_tail_record(CpuState *s, uint32_t target, RecompFn fn,
                         uint32_t restore_sp, uint32_t owes_restore);
void sr_llvm_tail_drain(CpuState *s);

#endif /* SR_LLVM_ABI_H */

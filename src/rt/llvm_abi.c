// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Named entry points over recomp.h's generated-code macros and inline helpers,
// for the LLVM backend's IR (#330). See llvm_abi.h. Each body is the macro or
// helper the C chunks expand, with the same arguments; always_inline lets the
// linked bitcode collapse into the calling body even at -O0, where the C chunks
// expand the macros in place.

#include "llvm_abi.h"

#define SR_LLVM_ABI __attribute__((always_inline)) inline

SR_LLVM_ABI void sr_llvm_begin(CpuState *s, uint32_t pc, uint32_t op) {
    (void)s; (void)pc; (void)op;
    sr_begin(s, pc, op);
}

SR_LLVM_ABI void sr_llvm_end(CpuState *s, uint32_t mem_addr, int32_t mem_size) {
    (void)s; (void)mem_addr; (void)mem_size;
    sr_end(s, mem_addr, mem_size);
}

SR_LLVM_ABI void sr_llvm_yield(CpuState *s, uint32_t target_pc) {
    SR_YIELD(s, target_pc);
}

SR_LLVM_ABI void sr_llvm_host_stack_check(uint32_t entry, void *frame) {
    SR_HOST_STACK_CHECK_FRAME(entry, (uintptr_t)frame);
}

SR_LLVM_ABI uint32_t sr_llvm_r8(uint32_t addr) { return MEM_R8(addr); }
SR_LLVM_ABI uint32_t sr_llvm_r16(uint32_t addr) { return MEM_R16(addr); }
SR_LLVM_ABI uint32_t sr_llvm_r32(uint32_t addr) { return MEM_R32(addr); }

SR_LLVM_ABI void sr_llvm_w8_pc(uint32_t addr, uint32_t value, uint32_t pc) {
    MEM_W8_PC(addr, value, pc);
}

SR_LLVM_ABI void sr_llvm_w16_pc(uint32_t addr, uint32_t value, uint32_t pc) {
    MEM_W16_PC(addr, value, pc);
}

SR_LLVM_ABI void sr_llvm_w32_pc(uint32_t addr, uint32_t value, uint32_t pc) {
    MEM_W32_PC(addr, value, pc);
}

SR_LLVM_ABI void sr_llvm_return_capture(CpuState *s, uint32_t target) {
    SR_AOT_RETURN_CAPTURE(s, target);
}

SR_LLVM_ABI void sr_llvm_tail_record(CpuState *s, uint32_t target, RecompFn fn,
                                     uint32_t restore_sp, uint32_t owes_restore) {
    sr_tail_record(s, target, fn, restore_sp, owes_restore);
}

SR_LLVM_ABI void sr_llvm_tail_drain(CpuState *s) {
    SR_TAIL_DRAIN(s);
}

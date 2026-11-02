// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * C interface to the reference interpreter (src/ref/interp.h) for the snapshot runner
 * (src/rt/snapshot_runner.c, issue #1842).
 *
 * The wrapper adds no semantics to ref::Run: it steps the interpreter one instruction at a
 * time and, between instructions, ends the run when the next instruction to fetch is the
 * return sentinel (the call returned) or an import stub address the caller named (the
 * call reached an import). Every other stop is the interpreter's own StopReason.
 */
#ifndef NK_SNAPSHOT_REF_H
#define NK_SNAPSHOT_REF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The guest RAM the reference models: ref::Memory's 64 MiB at 0x08000000. */
#define NK_REF_RAM_BASE 0x08000000u
#define NK_REF_RAM_SIZE 0x04000000u
/* The reference CpuState is layout-identical to the runtime's (both pin 1000 bytes). */
#define NK_REF_CPUSTATE_SIZE 1000u

typedef enum {
    NK_REF_RETURNED = 0,     /* the next fetch is the return sentinel */
    NK_REF_IMPORT,           /* the next fetch is a named import stub */
    NK_REF_SYSCALL,          /* a syscall instruction (StopReason::kSyscall) */
    NK_REF_UNIMPLEMENTED,    /* StopReason::kUnimplemented */
    NK_REF_MEMORY_FAULT,     /* StopReason::kMemoryFault */
    NK_REF_BREAK,            /* StopReason::kBreak */
    NK_REF_STEP_LIMIT,       /* the step budget ran out */
} NkRefStop;

typedef struct {
    NkRefStop stop;
    uint32_t pc;            /* RETURNED/IMPORT: the next fetch; others: the instruction */
    uint32_t op;            /* the instruction word, for SYSCALL/UNIMPLEMENTED/FAULT/BREAK */
    uint32_t fault_addr;    /* MEMORY_FAULT: the address the access used */
    uint64_t executed;      /* instructions executed */
} NkRefResult;

typedef struct NkRef NkRef;

NkRef *nk_ref_create(void);
void nk_ref_destroy(NkRef *ref);
/* The NK_REF_RAM_SIZE bytes of reference RAM, guest address NK_REF_RAM_BASE first. */
uint8_t *nk_ref_ram(NkRef *ref);

/* Run from cpu (NK_REF_CPUSTATE_SIZE bytes, updated in place) starting at entry_pc. */
void nk_ref_run(NkRef *ref, void *cpu, uint32_t entry_pc, uint32_t return_pc,
                const uint32_t *import_stubs, uint32_t import_count,
                uint64_t max_steps, NkRefResult *out);

/* A call inside a running program (src/rt/verify.c, issue #1843). The call is over when the
 * next fetch is return_pc with $sp equal to return_sp, so a recursive call that reaches
 * its own call site's return address deeper in the stack does not end it early. Imports
 * are not named: a syscall instruction (an import stub's delay slot included) is a stop. */
void nk_ref_run_call(NkRef *ref, void *cpu, uint32_t entry_pc, uint32_t return_pc,
                     uint32_t return_sp, uint64_t max_steps, NkRefResult *out);

#ifdef __cplusplus
}
#endif

#endif /* NK_SNAPSHOT_REF_H */

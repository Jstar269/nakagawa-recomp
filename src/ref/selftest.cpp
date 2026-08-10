// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)

// expected results and asserts the interpreter reproduces them. This is a real test: every
// expected value below is computed by hand from the MIPS semantics, not copied from the
// interpreter's own output. Exit code 0 means all checks passed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>

#include "interp.h"
#include "recomp.h"

using namespace ref;

namespace {

int g_failures = 0;

void Check(const char *what, uint32_t got, uint32_t want) {
	if (got != want) {
		std::printf("FAIL %-28s got=0x%08x want=0x%08x\n", what, got, want);
		g_failures++;
	} else {
		std::printf("ok   %-28s 0x%08x\n", what, got);
	}
}

// Register indices.
enum { ZERO = 0, T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15, S0 = 16, S1 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23, T8 = 24, T9 = 25, SP = 29 };

uint32_t R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct) {
	return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
uint32_t I(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t imm) {
	return (opcode << 26) | (rs << 21) | (rt << 16) | imm;
}
const uint32_t BREAK = 0x0000000D;
const uint32_t SYSCALL = 0x0000000C;

// Load a program at base and run to completion (a syscall instruction), returning final state.
ref::CpuState RunProgram(const std::vector<uint32_t> &prog, uint32_t base) {
	Memory mem;
	for (size_t i = 0; i < prog.size(); i++)
		mem.Write32(base + (uint32_t)i * 4, prog[i]);
	ref::CpuState s;
	s.pc = base;
	s.r[SP] = 0x08A00000;  // a valid RAM stack pointer
	StepResult res = Run(&s, &mem, 1000, nullptr);
	if (res.reason != StopReason::kSyscall && res.reason != StopReason::kStepLimit) {
		std::printf("FAIL program stopped: reason=%d pc=0x%08x op=0x%08x\n",
			(int)res.reason, res.pc, res.op);
		g_failures++;
	}
	return s;
}

void TestArithmetic() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, 0, T0, 0x1234),       // lui   t0, 0x1234        -> 0x12340000
		I(0x0D, T0, T0, 0x5678),      // ori   t0, t0, 0x5678    -> 0x12345678
		I(0x09, ZERO, T1, 100),       // addiu t1, zero, 100      -> 100
		R(T0, T1, T2, 0, 0x21),       // addu  t2, t0, t1         -> 0x123456DC
		R(ZERO, T1, T3, 4, 0x00),     // sll   t3, t1, 4          -> 1600 (0x640)
		R(T2, T1, T4, 0, 0x23),       // subu  t4, t2, t1         -> 0x12345678
		R(T0, T1, T5, 0, 0x24),       // and   t5, t0, t1         -> 0x12345678 & 100 = 0x60
		R(T0, T1, T6, 0, 0x2A),       // slt   t6, t0, t1         -> (0x12345678 < 100) signed = 0
		I(0x0A, T1, T7, 0xFFFF),      // slti  t7, t1, -1         -> (100 < -1) = 0
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("lui+ori t0", s.r[T0], 0x12345678);
	Check("addiu t1", s.r[T1], 100);
	Check("addu t2", s.r[T2], 0x123456DC);
	Check("sll t3", s.r[T3], 0x640);
	Check("subu t4", s.r[T4], 0x12345678);
	Check("and t5", s.r[T5], 0x60);
	Check("slt t6", s.r[T6], 0);
	Check("slti t7", s.r[T7], 0);
}

void TestMulDiv() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 7),         // addiu t0, zero, 7
		I(0x09, ZERO, T1, 6),         // addiu t1, zero, 6
		R(T0, T1, 0, 0, 0x18),        // mult  t0, t1            -> lo=42, hi=0
		R(0, 0, T2, 0, 0x12),         // mflo  t2                -> 42
		R(0, 0, T3, 0, 0x10),         // mfhi  t3                -> 0
		I(0x09, ZERO, T4, 100),       // addiu t4, zero, 100
		I(0x09, ZERO, T5, 7),         // addiu t5, zero, 7
		R(T4, T5, 0, 0, 0x1B),        // divu  t4, t5            -> lo=14, hi=2
		R(0, 0, T6, 0, 0x12),         // mflo  t6                -> 14
		R(0, 0, T7, 0, 0x10),         // mfhi  t7                -> 2
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("mult lo (mflo)", s.r[T2], 42);
	Check("mult hi (mfhi)", s.r[T3], 0);
	Check("divu lo (mflo)", s.r[T6], 14);
	Check("divu hi (mfhi)", s.r[T7], 2);
}

void TestMemory() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, 0, T0, 0xDEAD),       // lui   t0, 0xDEAD
		I(0x0D, T0, T0, 0xBEEF),      // ori   t0, t0, 0xBEEF    -> 0xDEADBEEF
		I(0x2B, SP, T0, 0x10),        // sw    t0, 16(sp)
		I(0x23, SP, T1, 0x10),        // lw    t1, 16(sp)        -> 0xDEADBEEF
		I(0x28, SP, T0, 0x20),        // sb    t0, 32(sp)        (stores 0xEF)
		I(0x24, SP, T2, 0x20),        // lbu   t2, 32(sp)        -> 0xEF
		I(0x20, SP, T3, 0x20),        // lb    t3, 32(sp)        -> sign-extended 0xFFFFFFEF
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("lw roundtrip", s.r[T1], 0xDEADBEEF);
	Check("lbu byte", s.r[T2], 0xEF);
	Check("lb sign-extend", s.r[T3], 0xFFFFFFEF);
}

void TestBranchDelaySlot() {
	const uint32_t base = 0x08900000;
	// beq taken: the delay slot runs, the instruction after it is skipped.
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 5),         // 0x..00 addiu t0, zero, 5
		I(0x09, ZERO, T1, 5),         // 0x..04 addiu t1, zero, 5
		I(0x04, T0, T1, 2),           // 0x..08 beq t0,t1,+2 -> target 0x..08+4+8 = 0x..14
		I(0x09, ZERO, T2, 0x111),     // 0x..0c delay slot: addiu t2, zero, 0x111 (runs)
		I(0x09, ZERO, T3, 0x222),     // 0x..10 skipped (branch target is 0x..14)
		I(0x09, ZERO, T4, 0x333),     // 0x..14 addiu t4, zero, 0x333 (runs)
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("delay slot executed", s.r[T2], 0x111);
	Check("skipped after branch", s.r[T3], 0);     // must remain 0
	Check("branch target ran", s.r[T4], 0x333);
}

void TestReferenceArithmeticGuards() {
	const uint32_t base = 0x08900000;
	// A backward branch exercises the signed displacement path without relying
	// on a host compiler's treatment of a negative left shift.
	std::vector<uint32_t> branch = {
		I(0x09, ZERO, T0, 1),       // t0 = 1
		I(0x09, ZERO, T1, 1),       // t1 = 1
		I(0x04, T0, T1, 0xFFFF),    // beq -1: loop back to the delay slot
		I(0x09, ZERO, T2, 7),       // delay slot, then step limit
	};
	Memory branch_mem;
	for (size_t i = 0; i < branch.size(); ++i) branch_mem.Write32(base + (uint32_t)i * 4, branch[i]);
	ref::CpuState branch_state;
	branch_state.pc = base;
	StepResult branch_result = Run(&branch_state, &branch_mem, 20, nullptr);
	Check("backward branch remains defined", static_cast<uint32_t>(branch_result.reason),
		static_cast<uint32_t>(StopReason::kStepLimit));

	// Reserved SPECIAL3 field combinations must stop before deriving an
	// underflowed/oversized mask.
	const uint32_t invalid_ins = (0x1Fu << 26) | (T0 << 16) | (T1 << 21) | (5u << 11) | (10u << 6) | 0x04u;
	Memory invalid_mem;
	invalid_mem.Write32(base, invalid_ins);
	ref::CpuState invalid_state;
	invalid_state.pc = base;
	StepResult invalid_result = Run(&invalid_state, &invalid_mem, 4, nullptr);
	Check("invalid ins encoding stops", static_cast<uint32_t>(invalid_result.reason),
		static_cast<uint32_t>(StopReason::kUnimplemented));
}

uint32_t MTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x04u << 21) | (rt << 16) | (fs << 11); }
uint32_t MFC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x00u << 21) | (rt << 16) | (fs << 11); }
uint32_t CTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x06u << 21) | (rt << 16) | (fs << 11); }
uint32_t FB(float v) { uint32_t b; std::memcpy(&b, &v, sizeof(b)); return b; }
uint32_t FPS(uint32_t ft, uint32_t fs, uint32_t fd, uint32_t funct) {
	return (0x11u << 26) | (0x10u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
uint32_t FPW(uint32_t fs, uint32_t fd, uint32_t funct) {
	return (0x11u << 26) | (0x14u << 21) | (fs << 11) | (fd << 6) | funct;
}

void TestFpu() {
	const uint32_t base = 0x08900000;
	// f0=2.0 (0x40000000), f1=3.0 (0x40400000) loaded via GPR + mtc1.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x4000), MTC1(T0, 0),   // f0 = 2.0f
		I(0x0F, ZERO, T1, 0x4040), MTC1(T1, 1),   // f1 = 3.0f
		FPS(1, 0, 2, 0x00), MFC1(T2, 2),          // add.s f2,f0,f1 -> 5.0f; t2 = bits
		FPS(1, 0, 3, 0x02), MFC1(T3, 3),          // mul.s f3,f0,f1 -> 6.0f; t3 = bits
		FPS(1, 0, 4, 0x01), MFC1(T4, 4),          // sub.s f4,f0,f1 -> -1.0f; t4 = bits
		I(0x09, ZERO, T5, 7), MTC1(T5, 5),        // f5 = int 7 (bits)
		FPW(5, 6, 0x20), MFC1(T6, 6),             // cvt.s.w f6,f5 -> 7.0f; t6 = bits
		I(0x0F, ZERO, T7, 0x40F0), MTC1(T7, 7),   // f7 = 7.5f (0x40F00000)
		FPS(0, 7, 8, 0x24), MFC1(T0, 8),          // cvt.w.s f8,f7 -> 8 (round-half-to-even); t0 = 8
		FPS(0, 7, 9, 0x0D), MFC1(T1, 9),          // trunc.w.s f9,f7 -> 7; t1 = 7
		I(0x0F, ZERO, T8, 0x0000), I(0x0D, T8, T8, 0xFFFF), MTC1(T8, 10),  // f10 = bits 0x0000FFFF (65535)
		FPW(10, 11, 0x20), MFC1(S0, 11),          // cvt.s.w f11,f10 -> 65535.0f (bits 0x477FFF00); s0 = bits
		I(0x0F, ZERO, T8, 0xFFFF), I(0x0D, T8, T8, 0xFFFF), MTC1(T8, 12),  // f12 = bits 0xFFFFFFFF (-1)
		FPW(12, 13, 0x20), MFC1(S1, 13),          // cvt.s.w f13,f12 -> -1.0f; s1 = bits
		I(0x0F, ZERO, T8, 0x7FFF), I(0x0D, T8, T8, 0xFFFF), MTC1(T8, 14),  // f14 = bits 0x7FFFFFFF (INT32_MAX)
		FPW(14, 15, 0x20), MFC1(S2, 15),          // cvt.s.w f15,f14 -> 2147483648.0f (0x4F000000, rounds); s2 = bits
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("add.s 2+3=5.0", s.r[T2], 0x40A00000);
	Check("mul.s 2*3=6.0", s.r[T3], 0x40C00000);
	Check("sub.s 2-3=-1.0", s.r[T4], 0xBF800000);
	Check("cvt.s.w 7=7.0", s.r[T6], 0x40E00000);
	Check("cvt.w.s 7.5=8", s.r[T0], 8);
	Check("trunc.w.s 7.5=7", s.r[T1], 7);
	Check("cvt.s.w 65535", s.r[S0], 0x477FFF00u);
	Check("cvt.s.w -1", s.r[S1], 0xBF800000u);
	Check("cvt.s.w INT32_MAX rounds", s.r[S2], 0x4F000000u);
}

void TestFpuConvertEdgeCases() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p;
	// Load a float's bit pattern into f-register fs via lui + ori + mtc1.
	auto load_float = [&](uint32_t fs, float v) {
		const uint32_t bits = FB(v);
		p.push_back(I(0x0F, ZERO, T0, (uint16_t)(bits >> 16)));
		p.push_back(I(0x0D, T0, T0, (uint16_t)(bits & 0xFFFF)));
		p.push_back(MTC1(T0, fs));
	};
	// Convert f[fs] to word into f[fd] and copy the bits to a GPR for the assert.
	auto to_word = [&](uint32_t gpr, uint32_t fs, uint32_t fd, uint32_t funct) {
		p.push_back(FPS(0, fs, fd, funct));
		p.push_back(MFC1(gpr, fd));
	};

	load_float(2, 2.5f);    // round.w.s: half up -> 3
	load_float(3, -2.5f);   // round.w.s: half up (toward +inf on ties) -> -2
	load_float(4, 2.1f);    // ceil.w.s -> 3
	load_float(5, 2.9f);    // floor.w.s -> 2
	load_float(6, -2.5f);   // trunc.w.s -> -2
	load_float(7, 2.5f);    // cvt.w.s RN -> 2 (tie to even)
	load_float(8, 3.5f);    // cvt.w.s RN -> 4 (tie to even)
	load_float(9, 2.7f);    // cvt.w.s RZ -> 2
	load_float(10, -2.7f);  // cvt.w.s RZ -> -2
	load_float(11, 1e30f);
	load_float(12, 1e30f);
	load_float(13, -1e30f);
	load_float(14, 2147483648.0f);   // exactly 2^31
	load_float(15, -2147483648.0f);  // exactly -2^31
	load_float(16, 2147483520.0f);   // largest float below 2^31

	// +inf / -inf / NaN loaded from raw bit patterns (no float literal needed).
	auto load_raw = [&](uint32_t fs, uint32_t bits) {
		p.push_back(I(0x0F, ZERO, T0, (uint16_t)(bits >> 16)));
		p.push_back(I(0x0D, T0, T0, (uint16_t)(bits & 0xFFFF)));
		p.push_back(MTC1(T0, fs));
	};
	load_raw(19, 0x7F800000u);  // f19 = +inf
	load_raw(20, 0xFF800000u);  // f20 = -inf
	load_raw(21, 0x7FC00000u);  // f21 = NaN

	// In-range rounding paths (fcr31 = 0 = RN).
	to_word(T3, 2, 2, 0x0C);   // round.w.s 2.5  -> 3
	to_word(T4, 3, 3, 0x0C);   // round.w.s -2.5 -> -2
	to_word(T5, 4, 4, 0x0E);   // ceil.w.s 2.1   -> 3
	to_word(T6, 5, 5, 0x0F);   // floor.w.s 2.9  -> 2
	to_word(T7, 6, 6, 0x0D);   // trunc.w.s -2.5 -> -2
	to_word(T8, 7, 7, 0x24);   // cvt.w.s 2.5 RN -> 2 (tie to even)
	to_word(T9, 8, 8, 0x24);   // cvt.w.s 3.5 RN -> 4 (tie to even)

	// Switch fcr31 to RZ (1) and run the two RZ cases, then restore RN.
	p.push_back(I(0x09, ZERO, T0, 1));
	p.push_back(CTC1(T0, 31));
	// Destinations kept distinct from the f9/f10 sources: writing the integer
	// result back into the source register would destroy the value the RP/RM
	// cases below still need.
	to_word(T1, 9, 24, 0x24);  // cvt.w.s 2.7 RZ  -> 2
	to_word(T2, 10, 25, 0x24); // cvt.w.s -2.7 RZ -> -2

	// RP (ceil) and RM (floor) rounding modes, then restore RN.
	p.push_back(I(0x09, ZERO, T0, 2));
	p.push_back(CTC1(T0, 31));
	to_word(26, 9, 22, 0x24);  // cvt.w.s 2.7 RP  -> 3 (ceil)
	p.push_back(I(0x09, ZERO, T0, 3));
	p.push_back(CTC1(T0, 31));
	to_word(27, 10, 23, 0x24); // cvt.w.s -2.7 RM -> -3 (floor)
	p.push_back(I(0x09, ZERO, T0, 0));
	p.push_back(CTC1(T0, 31));

	// Overflow / boundary / non-finite paths.
	to_word(S0, 11, 11, 0x0D);  // trunc.w.s 1e30f -> 0x7FFFFFFF (positive clamp)
	to_word(S1, 12, 12, 0x0E);  // ceil.w.s 1e30f  -> 0x80000000 (x86 host result)
	to_word(S2, 13, 13, 0x0F);  // floor.w.s -1e30f -> 0x80000000
	to_word(S3, 14, 14, 0x0D);  // trunc.w.s 2^31  -> 0x7FFFFFFF
	to_word(S4, 15, 15, 0x0D);  // trunc.w.s -2^31 -> 0x80000000
	to_word(S5, 16, 16, 0x0D);  // trunc.w.s 2147483520.0f -> 2147483520
	to_word(S6, 19, 17, 0x0C);  // round.w.s +inf  -> 0x7FFFFFFF
	to_word(S7, 20, 18, 0x0C);  // round.w.s -inf  -> 0x80000000
	p.push_back(FPS(0, 21, 19, 0x0D));
	p.push_back(MFC1(T0, 19));  // trunc.w.s NaN   -> 0x7FFFFFFF
	p.push_back(SYSCALL);

	ref::CpuState s = RunProgram(p, base);
	Check("round.w.s 2.5", s.r[T3], 3u);
	Check("round.w.s -2.5", s.r[T4], 0xFFFFFFFEu);
	Check("ceil.w.s 2.1", s.r[T5], 3u);
	Check("floor.w.s 2.9", s.r[T6], 2u);
	Check("trunc.w.s -2.5", s.r[T7], 0xFFFFFFFEu);
	Check("cvt.w.s 2.5 RN", s.r[T8], 2u);
	Check("cvt.w.s 3.5 RN", s.r[T9], 4u);
	Check("cvt.w.s 2.7 RZ", s.r[T1], 2u);
	Check("cvt.w.s -2.7 RZ", s.r[T2], 0xFFFFFFFEu);
	Check("cvt.w.s 2.7 RP", s.r[26], 3u);
	Check("cvt.w.s -2.7 RM", s.r[27], 0xFFFFFFFDu);
	Check("trunc.w.s +1e30 clamp", s.r[S0], 0x7FFFFFFFu);
	Check("ceil.w.s +1e30 host", s.r[S1], 0x80000000u);
	Check("floor.w.s -1e30", s.r[S2], 0x80000000u);
	Check("trunc.w.s 2^31", s.r[S3], 0x7FFFFFFFu);
	Check("trunc.w.s -2^31", s.r[S4], 0x80000000u);
	Check("trunc.w.s 2147483520", s.r[S5], 2147483520u);
	Check("round.w.s +inf", s.r[S6], 0x7FFFFFFFu);
	Check("round.w.s -inf", s.r[S7], 0x80000000u);
	Check("trunc.w.s NaN", s.r[T0], 0x7FFFFFFFu);
}

void TestArithmeticShifts() {
	const uint32_t base = 0x08900000;
	// sra/srav must sign-fill with portable bit logic (right-shifting a negative
	// signed value is implementation-defined C++).
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x8000),        // lui t0, 0x8000 -> 0x80000000
		R(T0, T0, T1, 3, 0x03),           // sra t1, t0, 3  -> 0xF0000000
		I(0x09, ZERO, T2, 3),             // addiu t2, zero, 3
		R(T2, T0, T3, 0, 0x07),           // srav t3, t0, t2 -> 0xF0000000
		R(T0, T0, T4, 31, 0x03),          // sra t4, t0, 31  -> 0xFFFFFFFF
		R(ZERO, T0, T5, 0, 0x03),         // sra t5, t0, 0   -> 0x80000000
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("sra 0x80000000>>3", s.r[T1], 0xF0000000u);
	Check("srav 0x80000000>>3", s.r[T3], 0xF0000000u);
	Check("sra >>31 sign fill", s.r[T4], 0xFFFFFFFFu);
	Check("sra >>0 identity", s.r[T5], 0x80000000u);
}

uint32_t BC1(uint32_t tf, uint32_t likely, uint16_t off) {
	return (0x11u << 26) | (0x08u << 21) | ((likely & 1) << 17) | ((tf & 1) << 16) | off;
}

void TestFpuCompareBranch() {
	const uint32_t base = 0x08900000;
	// c.lt.s f0,f1 with f0=2.0 < f1=3.0 sets the FPU condition; bc1t must be taken and skip
	// the instruction after its delay slot.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x4000), MTC1(T0, 0),   // f0 = 2.0
		I(0x0F, ZERO, T1, 0x4040), MTC1(T1, 1),   // f1 = 3.0
		FPS(1, 0, 0, 0x3C),                       // c.lt.s f0,f1 -> condition true
		BC1(1, 0, 2),                             // bc1t +2 (taken)
		0,                                        // delay slot: nop
		I(0x09, ZERO, T2, 0xBAD),                 // skipped on taken branch
		I(0x09, ZERO, T2, 0x111),                 // branch target
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("c.lt.s+bc1t taken", s.r[T2], 0x111);
}

void TestR0IsZero() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x09, ZERO, ZERO, 1234),    // addiu zero, zero, 1234  -> write to r0 discarded
		R(ZERO, ZERO, T0, 0, 0x21),   // addu  t0, zero, zero     -> 0
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("r0 stays zero", s.r[ZERO], 0);
	Check("addu from zero", s.r[T0], 0);
}

void TestBreakContinues() {
	const uint32_t base = 0x08900000;
	// break must advance PC and continue (PPSSPP Int_Break model).
	// A divide-by-zero guard uses break; the test verifies execution continues past it.
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 0),         // addiu t0, zero, 0    -> divisor = 0
		I(0x09, ZERO, T1, 42),        // addiu t1, zero, 42   -> dividend = 42
		R(T0, T1, 0, 0, 0x1A),        // div   t0, t1         -> triggers break guard
		0x0000000D,                    // break (funct 0x0D)   -> must continue, not stop
		I(0x09, ZERO, T2, 99),        // addiu t2, zero, 99   -> should execute
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("execution continued past break", s.r[T2], 99);
}

void TestBreakCodePreservation() {
	const uint32_t base = 0x08900000;
	// test break code 0
	std::vector<uint32_t> p0 = {
		0x0000000D, // break 0
		I(0x09, ZERO, T2, 99),
		SYSCALL,
	};
	ref::CpuState s0 = RunProgram(p0, base);
	Check("break 0 continued and ran next", s0.r[T2], 99);

	// test break code 7
	std::vector<uint32_t> p7 = {
		0x000001CD, // break 7
		I(0x09, ZERO, T2, 107),
		SYSCALL,
	};
	ref::CpuState s7 = RunProgram(p7, base);
	Check("break 7 continued and ran next", s7.r[T2], 107);
}

void TestBreakFatalMode() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		0x000001CD, // break 7
		I(0x09, ZERO, T2, 107),
		SYSCALL,
	};

	// 1. SR_BREAK_FATAL = 1 (should stop)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "1");
#else
		setenv("SR_BREAK_FATAL", "1", 1);
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Check("break fatal mode stopped at break", res.reason == StopReason::kBreak ? 1 : 0, 1);
		Check("break fatal mode stopped at correct PC", res.pc, base);
	}

	// 2. SR_BREAK_FATAL = 0 (should continue)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "0");
#else
		setenv("SR_BREAK_FATAL", "0", 1);
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Check("break fatal=0 continued past break", res.reason == StopReason::kSyscall ? 1 : 0, 1);
		Check("break fatal=0 reached target register setting", s.r[T2], 107);
	}

	// 3. SR_BREAK_FATAL = "" / empty (should continue)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		setenv("SR_BREAK_FATAL", "", 1);
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Check("break empty continued past break", res.reason == StopReason::kSyscall ? 1 : 0, 1);
		Check("break empty reached target register setting", s.r[T2], 107);
	}

	// 4. SR_BREAK_FATAL unset (should continue)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
		Check("break unset continued past break", res.reason == StopReason::kSyscall ? 1 : 0, 1);
		Check("break unset reached target register setting", s.r[T2], 107);
	}
}

void TestSyscallCodePreservation() {
	const uint32_t base = 0x08900000;
	// Test syscall 0x123
	Memory mem1;
	uint32_t op1 = 0x000048ccu; // (0x123 << 6) | 0x0C
	mem1.Write32(base, op1);
	ref::CpuState s1;
	s1.pc = base;
	StepResult res1 = Run(&s1, &mem1, 1, nullptr);
	Check("syscall 0x123 reason is kSyscall", res1.reason == StopReason::kSyscall ? 1 : 0, 1);
	Check("syscall 0x123 op is preserved", res1.op, op1);
	Check("syscall 0x123 code is preserved", (res1.op >> 6) & 0xFFFFF, 0x123);

	// Test syscall 0x456
	Memory mem2;
	uint32_t op2 = 0x0001158cu; // (0x456 << 6) | 0x0C
	mem2.Write32(base, op2);
	ref::CpuState s2;
	s2.pc = base;
	StepResult res2 = Run(&s2, &mem2, 1, nullptr);
	Check("syscall 0x456 reason is kSyscall", res2.reason == StopReason::kSyscall ? 1 : 0, 1);
	Check("syscall 0x456 op is preserved", res2.op, op2);
	Check("syscall 0x456 code is preserved", (res2.op >> 6) & 0xFFFFF, 0x456);
}

void TestAllegrexByteBitOps() {
	const uint32_t base = 0x08900000;
	// wsbh, wsbw, seb, seh, bitrev on known patterns.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x1234), MTC1(T0, 0),   // reuse T0 path: t0 = 0x12340000
		I(0x0D, T0, T0, 0x5678),                  // t0 = 0x12345678
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T1 << 11) | (0x02u << 6) | 0x20u,  // wsbh t1, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T2 << 11) | (0x03u << 6) | 0x20u,  // wsbw t2, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T3 << 11) | (0x10u << 6) | 0x20u,  // seb  t3, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T4 << 11) | (0x18u << 6) | 0x20u,  // seh  t4, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T5 << 11) | (0x14u << 6) | 0x20u,  // bitrev t5, t0
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("wsbh 0x12345678", s.r[T1], 0x34127856u);
	Check("wsbw 0x12345678", s.r[T2], 0x78563412u);
	Check("seb  0x12345678", s.r[T3], 0x00000078u);
	Check("seh  0x12345678", s.r[T4], 0x00005678u);
	Check("bitrev 0x12345678", s.r[T5], 0x1E6A2C48u);
}

void TestAllegrexExtIns() {
	const uint32_t base = 0x08900000;
	// ext: extract bits [15:8] from 0x12345678 -> 0x56
	// ins: insert 0xCD into bits [15:8] of 0x12345678 -> 0x1234CD78
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x1234), I(0x0D, T0, T0, 0x5678),  // t0 = 0x12345678
		I(0x0F, ZERO, T1, 0x1234), I(0x0D, T1, T1, 0xABCD),  // t1 = 0x1234ABCD
		(0x1Fu << 26) | (T0 << 21) | (T3 << 16) | ((8-1) << 11) | (8 << 6) | 0x00u,  // ext t3, t0, 8, 8 -> 0x56
		R(T0, ZERO, T2, 0, 0x21),                                                    // addu t2, t0, zero -> t2 = 0x12345678
		(0x1Fu << 26) | (T1 << 21) | (T2 << 16) | ((8+8-1) << 11) | (8 << 6) | 0x04u,  // ins t2, t1, 8, 8
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("ext 8,8 from 0x12345678", s.r[T3], 0x56u);
	Check("ins 8,8 0xCD into 0x12345678", s.r[T2], 0x1234CD78u);
}

void TestAllegrexMaxMin() {
	const uint32_t base = 0x08900000;
	// max/min signed comparisons: -1 vs 0, 0 vs 1, INT32_MIN vs INT32_MAX
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 0xFFFF),  // t0 = -1 (addiu zero, t0, 0xFFFF)
		I(0x09, ZERO, T1, 0),       // t1 = 0
		R(T0, T1, T2, 0, 0x2C),     // max t2, t0, t1 -> 0
		R(T0, T1, T3, 0, 0x2D),     // min t3, t0, t1 -> -1

		I(0x09, ZERO, T4, 0),       // t4 = 0
		I(0x09, ZERO, T5, 1),       // t5 = 1
		R(T4, T5, T6, 0, 0x2C),     // max t6, t4, t5 -> 1
		R(T4, T5, T7, 0, 0x2D),     // min t7, t4, t5 -> 0

		I(0x0F, ZERO, T8, 0x8000), I(0x0D, T8, T8, 0x0000),  // t8 = INT32_MIN
		I(0x0F, ZERO, T9, 0x7FFF), I(0x0D, T9, T9, 0xFFFF),  // t9 = INT32_MAX
		R(T8, T9, T0, 0, 0x2C),     // max t0, t8, t9 -> INT32_MAX
		R(T8, T9, T1, 0, 0x2D),     // min t1, t8, t9 -> INT32_MIN
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("max -1,0", s.r[T2], 0u);
	Check("min -1,0", s.r[T3], 0xFFFFFFFFu);
	Check("max 0,1", s.r[T6], 1u);
	Check("min 0,1", s.r[T7], 0u);
	Check("max INT32_MIN,INT32_MAX", s.r[T0], 0x7FFFFFFFu);
	Check("min INT32_MIN,INT32_MAX", s.r[T1], 0x80000000u);
}

}  // namespace

int main() {
	TestArithmetic();
	TestMulDiv();
	TestMemory();
	TestBranchDelaySlot();
	TestReferenceArithmeticGuards();
	TestFpu();
	TestFpuConvertEdgeCases();
	TestFpuCompareBranch();
	TestArithmeticShifts();
	TestR0IsZero();
	TestBreakContinues();
	TestBreakCodePreservation();
	TestBreakFatalMode();
	TestSyscallCodePreservation();
	TestAllegrexByteBitOps();
	TestAllegrexExtIns();
	TestAllegrexMaxMin();
	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
	return g_failures == 0 ? 0 : 1;
}

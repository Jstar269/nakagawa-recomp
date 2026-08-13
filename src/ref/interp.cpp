// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.


#include "interp.h"
#include "../rt/fp_convert.h"
#include "../rt/strbuf.h"

#include <cmath>
#include <cstring>
#include <cstdlib>

namespace ref {

// ---- Trace sink ----------------------------------------------------------------------

void TraceSink::Header(const char *target, uint32_t start_pc) {
	std::fprintf(out_, "# psp-recomp trace v1 oracle=interp target=%s start_pc=0x%08x\n",
		target ? target : "unknown", start_pc);
}

void TraceSink::InitDump(const CpuState *s) {
	std::fprintf(out_, "# init");
	for (int i = 0; i < 32; i++)
		if (s->r[i] != 0) std::fprintf(out_, " r%d=0x%08x", i, s->r[i]);
	for (int i = 0; i < 32; i++)
		if (s->fi[i] != 0) std::fprintf(out_, " f%d=0x%08x", i, s->fi[i]);
	if (s->hi != 0) std::fprintf(out_, " hi=0x%08x", s->hi);
	if (s->lo != 0) std::fprintf(out_, " lo=0x%08x", s->lo);
	if (s->fcr31 != 0) std::fprintf(out_, " fcr31=0x%08x", s->fcr31);
	std::fprintf(out_, "\n");
}

void TraceSink::BeginStep(const CpuState *s, uint32_t pc, uint32_t op) {
	std::memcpy(r_, s->r, sizeof(r_));
	std::memcpy(fi_, s->fi, sizeof(fi_));
	hi_ = s->hi;
	lo_ = s->lo;
	fcr31_ = s->fcr31;
	pc_ = pc;
	op_ = op;
}

void TraceSink::EndStep(const CpuState *s, uint32_t mem_addr, int mem_size, const Memory *mem) {
	char line[4096];
	/* Checked appends: snprintf() returns the "would have been" length, so a
	 * truncated token must not be allowed to advance the cursor past the
	 * buffer (sr_buf_append clamps to the last NUL slot).  The line stays
	 * byte-comparable with the recompiled-code trace whenever nothing
	 * truncates. */
	size_t n = 0;
	n = sr_buf_append(line, sizeof(line), n, "%llu pc=0x%08x op=0x%08x", step_, pc_, op_);
	for (int i = 1; i < 32; i++)
		if (s->r[i] != r_[i])
			n = sr_buf_append(line, sizeof(line), n, " r%d=0x%08x", i, s->r[i]);
	if (s->hi != hi_)
		n = sr_buf_append(line, sizeof(line), n, " hi=0x%08x", s->hi);
	if (s->lo != lo_)
		n = sr_buf_append(line, sizeof(line), n, " lo=0x%08x", s->lo);
	for (int i = 0; i < 32; i++)
		if (s->fi[i] != fi_[i])
			n = sr_buf_append(line, sizeof(line), n, " f%d=0x%08x", i, s->fi[i]);
	if (s->fcr31 != fcr31_)
		n = sr_buf_append(line, sizeof(line), n, " fcr31=0x%08x", s->fcr31);
	if (mem_size == 1)
		n = sr_buf_append(line, sizeof(line), n, " m8[0x%08x]=0x%02x", mem_addr, mem->Read8(mem_addr));
	else if (mem_size == 2)
		n = sr_buf_append(line, sizeof(line), n, " m16[0x%08x]=0x%04x", mem_addr, mem->Read16(mem_addr));
	else if (mem_size == 4)
		n = sr_buf_append(line, sizeof(line), n, " m32[0x%08x]=0x%08x", mem_addr, mem->Read32(mem_addr));
	/* sr_buf_append guarantees n < sizeof(line), so the newline slot and the
	 * fwrite byte count are always in range. */
	line[n] = '\n';
	std::fwrite(line, 1, n + 1, out_);
	step_++;
}

// ---- Instruction field helpers -------------------------------------------------------

namespace {

inline uint32_t Rs(uint32_t op) { return (op >> 21) & 0x1F; }
inline uint32_t Rt(uint32_t op) { return (op >> 16) & 0x1F; }
inline uint32_t Rd(uint32_t op) { return (op >> 11) & 0x1F; }
inline uint32_t Sa(uint32_t op) { return (op >> 6) & 0x1F; }
inline uint32_t Funct(uint32_t op) { return op & 0x3F; }
inline int32_t SImm(uint32_t op) { return (int32_t)(int16_t)(op & 0xFFFF); }
inline uint32_t ZImm(uint32_t op) { return op & 0xFFFF; }

// MIPS branch displacement is a signed 16-bit word offset, but the guest PC
// addition wraps at 32 bits.  Convert to unsigned before scaling so a
// backwards branch never left-shifts a negative signed value (C++ UB).
inline uint32_t BranchTarget(uint32_t branch_pc, int32_t displacement) {
	return (branch_pc + 4u) + static_cast<uint32_t>(displacement) * 4u;
}

// Arithmetic right shift on a 32-bit word with defined behavior. Right-shifting
// a negative signed value is implementation-defined in C++, so the shift runs on
// the unsigned value with an explicit sign-fill (portable bit logic). Callers
// pass 0..31 (instruction shift fields are 5 bits); n == 0 is the identity.
inline uint32_t ArithShiftRight(uint32_t v, unsigned n) {
	if (n == 0) return v;
	const uint32_t shifted = v >> n;
	const uint32_t fill = (0u - (v >> 31)) << (32 - n);  // all-ones when the sign bit is set
	return shifted | fill;
}

inline void SetR(CpuState *s, uint32_t idx, uint32_t val) {
	if (idx != 0) s->r[idx] = val;
}

// Compute a store's address and access size from the opcode, or size 0 if not a store.
// Mirrors how the trace records memory writes (TRACE_FORMAT.md), so it matches the oracle.
void StoreInfo(const CpuState *s, uint32_t op, uint32_t *addr, int *size) {
	*size = 0;
	uint32_t opcode = op >> 26;
	switch (opcode) {
		case 0x28: *size = 1; break;   // sb
		case 0x29: *size = 2; break;   // sh
		case 0x2B: *size = 4; break;   // sw
		case 0x2A: *size = 4; break;   // swl (records the touched word)
		case 0x2E: *size = 4; break;   // swr
		case 0x39: *size = 4; break;   // swc1
		default: return;
	}
	*addr = s->r[Rs(op)] + SImm(op);
}

}  // namespace

// ---- Execute one instruction ---------------------------------------------------------

// Advances s->pc and sets branch bookkeeping. Returns kRunning on success, or a stop reason
// for syscall / unimplemented / break. Memory faults are detected by the caller via the
// Memory fault flag after load/store.
static StopReason Execute(CpuState *s, Memory *mem, uint32_t op) {
	const uint32_t opcode = op >> 26;
	const uint32_t branch_pc = s->pc;
	s->pc += 4;  // default sequential advance; branches override via next_pc/in_delay_slot

	auto take_branch = [&](bool cond) {
		if (cond) {
			s->next_pc = BranchTarget(branch_pc, SImm(op));
			s->in_delay_slot = true;
		}
	};
	auto skip_likely = [&](bool cond) {
		// Branch-likely: when taken behaves like a normal branch; when not taken the delay
		// slot is annulled (skipped).
		if (cond) {
			s->next_pc = BranchTarget(branch_pc, SImm(op));
			s->in_delay_slot = true;
		} else {
			s->pc += 4;  // skip the delay-slot instruction
		}
	};

	switch (opcode) {
		case 0x00: {  // SPECIAL
			const uint32_t funct = Funct(op);
			switch (funct) {
				case 0x00: SetR(s, Rd(op), s->r[Rt(op)] << Sa(op)); return StopReason::kRunning;          // sll
				case 0x02: SetR(s, Rd(op), s->r[Rt(op)] >> Sa(op)); return StopReason::kRunning;          // srl
				case 0x03: SetR(s, Rd(op), ArithShiftRight(s->r[Rt(op)], Sa(op))); return StopReason::kRunning;  // sra
				case 0x04: SetR(s, Rd(op), s->r[Rt(op)] << (s->r[Rs(op)] & 31)); return StopReason::kRunning;          // sllv
				case 0x06: SetR(s, Rd(op), s->r[Rt(op)] >> (s->r[Rs(op)] & 31)); return StopReason::kRunning;          // srlv
				case 0x07: SetR(s, Rd(op), ArithShiftRight(s->r[Rt(op)], s->r[Rs(op)] & 31)); return StopReason::kRunning;  // srav
				case 0x08: {  // jr
					s->next_pc = s->r[Rs(op)];
					s->in_delay_slot = true;
					return StopReason::kRunning;
				}
				case 0x09: {  // jalr
					SetR(s, Rd(op), branch_pc + 8);
					s->next_pc = s->r[Rs(op)];
					s->in_delay_slot = true;
					return StopReason::kRunning;
				}
				case 0x0A: if (s->r[Rt(op)] == 0) SetR(s, Rd(op), s->r[Rs(op)]); return StopReason::kRunning;  // movz
				case 0x0B: if (s->r[Rt(op)] != 0) SetR(s, Rd(op), s->r[Rs(op)]); return StopReason::kRunning;  // movn
				case 0x0C: return StopReason::kSyscall;   // syscall
			// break: PSP uses BREAK for assertions. Log/report and continue under compatibility
			// policy, unless SR_BREAK_FATAL is set to force halt (distinguishing from normal execution).
			// This is a controlled compatibility approximation, not full PSP BREAK exception emulation.
			case 0x0D: {
				uint32_t code = (op >> 6) & 0xFFFFF;
				std::fprintf(stderr, "[interp/cpu] BREAK 0x%x encountered at pc=0x%08x\n", code, branch_pc);
				const char *fatal = std::getenv("SR_BREAK_FATAL");
				if (fatal && fatal[0] != '\0' && std::strcmp(fatal, "0") != 0) {
					std::fprintf(stderr, "Fatal error: SR_BREAK_FATAL is set; stopping.\n");
					return StopReason::kBreak;
				}
				return StopReason::kRunning;
			}
				case 0x10: SetR(s, Rd(op), s->hi); return StopReason::kRunning;  // mfhi
				case 0x11: s->hi = s->r[Rs(op)]; return StopReason::kRunning;    // mthi
				case 0x12: SetR(s, Rd(op), s->lo); return StopReason::kRunning;  // mflo
				case 0x13: s->lo = s->r[Rs(op)]; return StopReason::kRunning;    // mtlo
				case 0x18: {  // mult
					int64_t prod = (int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)];
					s->lo = (uint32_t)prod; s->hi = (uint32_t)(prod >> 32); return StopReason::kRunning;
				}
				case 0x19: {  // multu
					uint64_t prod = (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)prod; s->hi = (uint32_t)(prod >> 32); return StopReason::kRunning;
				}
				case 0x1A: {  // div (semantics match PPSSPP Int_MulDivType exactly)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					if (a == (int32_t)0x80000000 && b == -1) { s->lo = 0x80000000; s->hi = 0xFFFFFFFF; }
					else if (b != 0) { s->lo = (uint32_t)(a / b); s->hi = (uint32_t)(a % b); }
					else { s->lo = a < 0 ? 1u : 0xFFFFFFFFu; s->hi = (uint32_t)a; }
					return StopReason::kRunning;
				}
				case 0x1B: {  // divu
					uint32_t a = s->r[Rs(op)], b = s->r[Rt(op)];
					if (b != 0) { s->lo = a / b; s->hi = a % b; }
					else { s->lo = a <= 0xFFFF ? 0xFFFFu : 0xFFFFFFFFu; s->hi = a; }
					return StopReason::kRunning;
				}
				case 0x20:    // add (PSP does not trap on overflow in practice for our targets)
				case 0x21: SetR(s, Rd(op), s->r[Rs(op)] + s->r[Rt(op)]); return StopReason::kRunning;  // addu
				case 0x22:    // sub
				case 0x23: SetR(s, Rd(op), s->r[Rs(op)] - s->r[Rt(op)]); return StopReason::kRunning;  // subu
				case 0x24: SetR(s, Rd(op), s->r[Rs(op)] & s->r[Rt(op)]); return StopReason::kRunning;  // and
				case 0x25: SetR(s, Rd(op), s->r[Rs(op)] | s->r[Rt(op)]); return StopReason::kRunning;  // or
				case 0x26: SetR(s, Rd(op), s->r[Rs(op)] ^ s->r[Rt(op)]); return StopReason::kRunning;  // xor
				case 0x27: SetR(s, Rd(op), ~(s->r[Rs(op)] | s->r[Rt(op)])); return StopReason::kRunning;  // nor
				case 0x2A: SetR(s, Rd(op), (int32_t)s->r[Rs(op)] < (int32_t)s->r[Rt(op)] ? 1 : 0); return StopReason::kRunning;  // slt
				case 0x2B: SetR(s, Rd(op), s->r[Rs(op)] < s->r[Rt(op)] ? 1 : 0); return StopReason::kRunning;  // sltu
				// Allegrex places these in SPECIAL (not in a separate SPECIAL2 page like
				// generic MIPS32): clz/clo at 0x16/0x17, madd/maddu at 0x1C/0x1D, max/min at
				// 0x2C/0x2D, msub/msubu at 0x2E/0x2F. Encodings verified against PPSSPP's
				// MIPSTables.cpp SPECIAL table.
				case 0x16: {  // clz rd, rs
					uint32_t v = s->r[Rs(op)];
					SetR(s, Rd(op), v == 0 ? 32 : (uint32_t)__builtin_clz(v)); return StopReason::kRunning;
				}
				case 0x17: {  // clo rd, rs
					uint32_t v = s->r[Rs(op)];
					SetR(s, Rd(op), v == 0xFFFFFFFF ? 32 : (uint32_t)__builtin_clz(~v)); return StopReason::kRunning;
				}
				case 0x1C: {  // madd
					uint64_t acc = ((uint64_t)s->hi << 32) | (uint32_t)s->lo;
					acc += (uint64_t)((int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)]);
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x1D: {  // maddu
					uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
					acc += (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x2C: {  // max rd, rs, rt (signed)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					SetR(s, Rd(op), (uint32_t)(a > b ? a : b)); return StopReason::kRunning;
				}
				case 0x2D: {  // min rd, rs, rt (signed)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					SetR(s, Rd(op), (uint32_t)(a < b ? a : b)); return StopReason::kRunning;
				}
				case 0x2E: {  // msub
					uint64_t acc = ((uint64_t)s->hi << 32) | (uint32_t)s->lo;
					acc -= (uint64_t)((int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)]);
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x2F: {  // msubu
					uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
					acc -= (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x01: {  // REGIMM
			const uint32_t rt = Rt(op);
			int32_t v = (int32_t)s->r[Rs(op)];
			switch (rt) {
				case 0x00: take_branch(v < 0); return StopReason::kRunning;   // bltz
				case 0x01: take_branch(v >= 0); return StopReason::kRunning;  // bgez
				case 0x02: skip_likely(v < 0); return StopReason::kRunning;   // bltzl
				case 0x03: skip_likely(v >= 0); return StopReason::kRunning;  // bgezl
				case 0x10: SetR(s, 31, branch_pc + 8); take_branch(v < 0); return StopReason::kRunning;   // bltzal
				case 0x11: SetR(s, 31, branch_pc + 8); take_branch(v >= 0); return StopReason::kRunning;  // bgezal
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x02: s->next_pc = (branch_pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2); s->in_delay_slot = true; return StopReason::kRunning;  // j
		case 0x03: SetR(s, 31, branch_pc + 8); s->next_pc = (branch_pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2); s->in_delay_slot = true; return StopReason::kRunning;  // jal
		case 0x04: take_branch(s->r[Rs(op)] == s->r[Rt(op)]); return StopReason::kRunning;  // beq
		case 0x05: take_branch(s->r[Rs(op)] != s->r[Rt(op)]); return StopReason::kRunning;  // bne
		case 0x06: take_branch((int32_t)s->r[Rs(op)] <= 0); return StopReason::kRunning;    // blez
		case 0x07: take_branch((int32_t)s->r[Rs(op)] > 0); return StopReason::kRunning;     // bgtz
		case 0x08:    // addi
		case 0x09: SetR(s, Rt(op), s->r[Rs(op)] + (uint32_t)SImm(op)); return StopReason::kRunning;  // addiu
		case 0x0A: SetR(s, Rt(op), (int32_t)s->r[Rs(op)] < SImm(op) ? 1 : 0); return StopReason::kRunning;  // slti
		case 0x0B: SetR(s, Rt(op), s->r[Rs(op)] < (uint32_t)SImm(op) ? 1 : 0); return StopReason::kRunning;  // sltiu
		case 0x0C: SetR(s, Rt(op), s->r[Rs(op)] & ZImm(op)); return StopReason::kRunning;  // andi
		case 0x0D: SetR(s, Rt(op), s->r[Rs(op)] | ZImm(op)); return StopReason::kRunning;  // ori
		case 0x0E: SetR(s, Rt(op), s->r[Rs(op)] ^ ZImm(op)); return StopReason::kRunning;  // xori
		case 0x0F: SetR(s, Rt(op), ZImm(op) << 16); return StopReason::kRunning;           // lui
		case 0x14: skip_likely(s->r[Rs(op)] == s->r[Rt(op)]); return StopReason::kRunning;  // beql
		case 0x15: skip_likely(s->r[Rs(op)] != s->r[Rt(op)]); return StopReason::kRunning;  // bnel
		case 0x16: skip_likely((int32_t)s->r[Rs(op)] <= 0); return StopReason::kRunning;    // blezl
		case 0x17: skip_likely((int32_t)s->r[Rs(op)] > 0); return StopReason::kRunning;     // bgtzl
		case 0x20: SetR(s, Rt(op), (uint32_t)(int32_t)(int8_t)mem->Read8(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;   // lb
		case 0x21: SetR(s, Rt(op), (uint32_t)(int32_t)(int16_t)mem->Read16(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning; // lh
		case 0x23: SetR(s, Rt(op), mem->Read32(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;  // lw
		case 0x24: SetR(s, Rt(op), mem->Read8(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;   // lbu
		case 0x25: SetR(s, Rt(op), mem->Read16(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;  // lhu
		case 0x28: mem->Write8(s->r[Rs(op)] + SImm(op), (uint8_t)s->r[Rt(op)]); return StopReason::kRunning;   // sb
		case 0x29: mem->Write16(s->r[Rs(op)] + SImm(op), (uint16_t)s->r[Rt(op)]); return StopReason::kRunning; // sh
		case 0x2B: mem->Write32(s->r[Rs(op)] + SImm(op), s->r[Rt(op)]); return StopReason::kRunning;  // sw
		case 0x22: {  // lwl: unaligned little-endian word left (high bytes)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t aligned = addr & ~3u;
			uint32_t rtv = s->r[Rt(op)];
			uint32_t word = mem->Read32(aligned);
			int shift = (addr & 3) * 8;
			// Little-endian LWL: for addr+k, take the high (4-k) bytes of the aligned
			// word and merge them into the low end of rtv.
			uint32_t mask = 0xFFFFFFFFu << (24 - shift);
			uint32_t result = (rtv & ~mask) | ((word << shift) & mask);
			SetR(s, Rt(op), result);
			return StopReason::kRunning;
		}
		case 0x26: {  // lwr: unaligned little-endian word right (low bytes)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t aligned = addr & ~3u;
			uint32_t rtv = s->r[Rt(op)];
			uint32_t word = mem->Read32(aligned);
			int shift = (addr & 3) * 8;
			// Little-endian LWR: for addr+k, take the low (k+1) bytes of the aligned
			// word and merge them into the high end of rtv.
			uint32_t mask = 0xFFFFFFFFu >> (24 - shift);
			uint32_t result = (rtv & ~mask) | ((word >> (24 - shift)) & mask);
			SetR(s, Rt(op), result);
			return StopReason::kRunning;
		}
		case 0x2A: {  // swl: store-word-left (high bytes of rt to low bytes of aligned word)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t aligned = addr & ~3u;
			uint32_t rtv = s->r[Rt(op)];
			uint32_t word = mem->Read32(aligned);
			int shift = (addr & 3) * 8;
			uint32_t mask = 0xFFFFFFFFu << (24 - shift);
			uint32_t result = (word & ~mask) | ((rtv >> shift) & mask);
			mem->Write32(aligned, result);
			return StopReason::kRunning;
		}
		case 0x2E: {  // swr: store-word-right (low bytes of rt to high bytes of aligned word)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t aligned = addr & ~3u;
			uint32_t rtv = s->r[Rt(op)];
			uint32_t word = mem->Read32(aligned);
			int shift = (addr & 3) * 8;
			uint32_t mask = 0xFFFFFFFFu >> (24 - shift);
			uint32_t result = (word & ~mask) | ((rtv << (24 - shift)) & mask);
			mem->Write32(aligned, result);
			return StopReason::kRunning;
		}
		case 0x30: {  // ll: load-linked (single-thread emulation: plain read with upper-bit set on failure)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			SetR(s, Rt(op), mem->Read32(addr));
			return StopReason::kRunning;
		}
		case 0x38: {  // sc: store-conditional (single-thread emulation: always succeeds)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			mem->Write32(addr, s->r[Rt(op)]);
			SetR(s, Rt(op), 1);  // success
			return StopReason::kRunning;
		}
		case 0x11: {  // COP1 (single-precision FPU)
			const uint32_t fmt = Rs(op);  // sub-op selector in the rs field
			const uint32_t ft = Rt(op);
			const uint32_t fs = Rd(op);   // bits 15:11
			const uint32_t fd = Sa(op);   // bits 10:6
			switch (fmt) {
				case 0x00: SetR(s, ft, s->fi[fs]); return StopReason::kRunning;   // mfc1
				case 0x02:  // cfc1
					SetR(s, ft, fs == 31 ? s->fcr31 : (fs == 0 ? 0x00003351u : 0u));
					return StopReason::kRunning;
				case 0x04: s->fi[fs] = s->r[ft]; return StopReason::kRunning;     // mtc1
				case 0x06: if (fs == 31) s->fcr31 = s->r[ft]; return StopReason::kRunning;  // ctc1
				case 0x08: {  // bc1f / bc1t / bc1fl / bc1tl
					bool tf = (op >> 16) & 1;
					bool likely = (op >> 17) & 1;
					bool cond = s->fpcond != 0;
					if (likely) skip_likely(cond == tf); else take_branch(cond == tf);
					return StopReason::kRunning;
				}
				case 0x10: {  // fmt = S
					const uint32_t funct = Funct(op);
					switch (funct) {
						case 0x00: s->f[fd] = s->f[fs] + s->f[ft]; return StopReason::kRunning;  // add.s
						case 0x01: s->f[fd] = s->f[fs] - s->f[ft]; return StopReason::kRunning;  // sub.s
						case 0x02: {  // mul.s; PPSSPP forces inf*0 to the positive canonical NaN
							float a = s->f[fs], b = s->f[ft];
							if ((std::isinf(a) && b == 0.0f) || (std::isinf(b) && a == 0.0f))
								s->fi[fd] = 0x7fc00000;
							else
								s->f[fd] = a * b;
							return StopReason::kRunning;
						}
						case 0x03: s->f[fd] = s->f[fs] / s->f[ft]; return StopReason::kRunning;  // div.s
						case 0x04: s->f[fd] = std::sqrt(s->f[fs]); return StopReason::kRunning;  // sqrt.s
						case 0x05: s->f[fd] = std::fabs(s->f[fs]); return StopReason::kRunning;  // abs.s
						case 0x06: s->f[fd] = s->f[fs]; return StopReason::kRunning;             // mov.s
						case 0x07: s->f[fd] = -s->f[fs]; return StopReason::kRunning;            // neg.s
						case 0x0C: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0C, s->fcr31); return StopReason::kRunning;  // round.w.s
						case 0x0D: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0D, s->fcr31); return StopReason::kRunning;  // trunc.w.s
						case 0x0E: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0E, s->fcr31); return StopReason::kRunning;  // ceil.w.s
						case 0x0F: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0F, s->fcr31); return StopReason::kRunning;  // floor.w.s
						case 0x24: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x24, s->fcr31); return StopReason::kRunning;  // cvt.w.s
						default:
							if (funct >= 0x30) {  // c.cond.s
								uint32_t cond = funct & 0xF;
								float a = s->f[fs], b = s->f[ft];
								bool unordered = std::isnan(a) || std::isnan(b);
								bool less = !unordered && a < b;
								bool equal = !unordered && a == b;
								bool result = (unordered && (cond & 1)) || (equal && (cond & 2)) || (less && (cond & 4));
								s->fpcond = result ? 1 : 0;  // PPSSPP stores the condition here, not in fcr31
								return StopReason::kRunning;
							}
							return StopReason::kUnimplemented;
					}
				}
				case 0x14: {  // fmt = W
					if (Funct(op) == 0x20) {
						// cvt.s.w: map the W register's bit pattern to its signed value
						// without an implementation-defined uint32_t-to-int32_t cast.
						s->f[fd] = (float)sr_u32_as_s32(s->fi[fs]);
					return StopReason::kRunning;
				}
					return StopReason::kUnimplemented;
				}
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x1F: {  // SPECIAL3
			const uint32_t funct = Funct(op);
			if (funct == 0x00) {  // ext rt, rs, pos, size
				uint32_t pos = Sa(op);
				uint32_t size = ((op >> 11) & 0x1F) + 1;
				if (pos + size > 32) return StopReason::kUnimplemented;
				uint32_t mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
				SetR(s, Rt(op), (s->r[Rs(op)] >> pos) & mask);
				return StopReason::kRunning;
			}
			if (funct == 0x04) {  // ins rt, rs, pos, size
				uint32_t pos = Sa(op);
				uint32_t msb = (op >> 11) & 0x1F;
				if (msb < pos) return StopReason::kUnimplemented;
				uint32_t size = msb - pos + 1;
				uint32_t mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << pos;
				SetR(s, Rt(op), (s->r[Rt(op)] & ~mask) | ((s->r[Rs(op)] << pos) & mask));
				return StopReason::kRunning;
			}
			if (funct == 0x20) {  // BSHFL: sub-op in sa field
				uint32_t sub = Sa(op);
				uint32_t v = s->r[Rt(op)];
				if (sub == 0x02) { SetR(s, Rd(op), ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF)); return StopReason::kRunning; }  // wsbh
				if (sub == 0x03) { SetR(s, Rd(op), ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | ((v >> 24) & 0x000000FFu)); return StopReason::kRunning; }  // wsbw
				if (sub == 0x10) { SetR(s, Rd(op), (uint32_t)(int32_t)(int8_t)v); return StopReason::kRunning; }   // seb
				if (sub == 0x18) { SetR(s, Rd(op), (uint32_t)(int32_t)(int16_t)v); return StopReason::kRunning; }  // seh
				if (sub == 0x14) { SetR(s, Rd(op), sr_bitrev(v)); return StopReason::kRunning; }  // bitrev
				return StopReason::kUnimplemented;
			}
			return StopReason::kUnimplemented;
		}
		case 0x31: s->fi[Rt(op)] = mem->Read32(s->r[Rs(op)] + SImm(op)); return StopReason::kRunning; // lwc1
		case 0x39: mem->Write32(s->r[Rs(op)] + SImm(op), s->fi[Rt(op)]); return StopReason::kRunning; // swc1
		default: return StopReason::kUnimplemented;
	}
}

StepResult Run(CpuState *s, Memory *mem, unsigned long long max_steps, TraceSink *sink) {
	for (unsigned long long i = 0; i < max_steps; i++) {
		// Clear the sticky fault flag at the start of each iteration. Without this,
		// a single transient fault (e.g. during speculative prefetch) would mark all
		// subsequent valid accesses as faulty forever.
		mem->ClearFault();
		uint32_t pc = s->pc;
		uint32_t op = mem->Read32(pc);
		if (mem->last_fault())
			return {StopReason::kMemoryFault, pc, op};

		uint32_t store_addr = 0;
		int store_size = 0;
		StoreInfo(s, op, &store_addr, &store_size);

		if (sink) sink->BeginStep(s, pc, op);
		bool was_in_delay_slot = s->in_delay_slot;
		StopReason reason = Execute(s, mem, op);
		if (s->in_delay_slot && was_in_delay_slot) {
			s->pc = s->next_pc;
			s->in_delay_slot = false;
		}
		if (sink) sink->EndStep(s, store_addr, store_size, mem);

		if (mem->last_fault())
			return {StopReason::kMemoryFault, pc, op};
		if (reason != StopReason::kRunning)
			return {reason, pc, op};
	}
	return {StopReason::kStepLimit, s->pc, 0};
}

}  // namespace ref

// Bit-reverse a 32-bit word (Allegrex bitrev).  This is a standalone definition
// for the selftest/run_elf builds; the runtime copy lives in recomp.c.
extern "C" uint32_t sr_bitrev(uint32_t x) {
    x = ((x >> 1) & 0x55555555) | ((x << 1) & 0xaaaaaaaa);
    x = ((x >> 2) & 0x33333333) | ((x << 2) & 0xcccccccc);
    x = ((x >> 4) & 0x0f0f0f0f) | ((x << 4) & 0xf0f0f0f0);
    x = ((x >> 8) & 0x00ff00ff) | ((x << 8) & 0xff00ff00);
    return (x >> 16) | (x << 16);
}

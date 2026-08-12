// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

//
// This interpreter is the counterpart to the PPSSPP oracle (ARCHITECTURE.md section 1, 10):
// it executes guest code from a controlled start state and emits the same per-instruction
// trace format (tools/TRACE_FORMAT.md) so its output can be diffed against PPSSPP. It is
// also the runtime fallback and the reference for the Phase 3 codegen.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ref {

// Guest RAM model. The PSP user RAM this title uses is 64 MB at 0x08000000..0x0BFFFFFF
// (the extended model the trace shows, with the stack near 0x0bffffff). Cached and uncached
// mirrors fold to the same bytes by masking; here a single linear buffer is indexed by the
// physical offset from the RAM base. Scratchpad and VRAM are added when a target needs them.
class Memory {
public:
	static constexpr uint32_t kRamBase = 0x08000000;
	static constexpr uint32_t kRamSize = 0x04000000;  // 64 MB
	static constexpr uint32_t kPhysMask = 0x1FFFFFFF;

	Memory() : ram_(kRamSize, 0) {}

	// Translate a guest virtual address to an index into the RAM buffer. Returns false for
	// addresses outside modeled RAM so callers trap instead of corrupting host memory.
	bool Translate(uint32_t addr, uint32_t *index) const {
		uint32_t phys = addr & kPhysMask;
		if (phys < kRamBase || phys >= kRamBase + kRamSize)
			return false;
		*index = phys - kRamBase;
		return true;
	}

	uint8_t *RamData() { return ram_.data(); }

	uint8_t Read8(uint32_t a) const { return Load<uint8_t>(a); }
	uint16_t Read16(uint32_t a) const { return Load<uint16_t>(a); }
	uint32_t Read32(uint32_t a) const { return Load<uint32_t>(a); }
	void Write8(uint32_t a, uint8_t v) { Store<uint8_t>(a, v); }
	void Write16(uint32_t a, uint16_t v) { Store<uint16_t>(a, v); }
	void Write32(uint32_t a, uint32_t v) { Store<uint32_t>(a, v); }

	bool last_fault() const { return fault_; }
	uint32_t fault_addr() const { return fault_addr_; }
	// Explicitly clear the sticky fault flag so the next access can succeed.
	void ClearFault() const { fault_ = false; fault_addr_ = 0; }

private:
	template <typename T> T Load(uint32_t addr) const {
		uint32_t idx;
		if (!Translate(addr, &idx) || idx + sizeof(T) > kRamSize) {
			fault_ = true;
			fault_addr_ = addr;
			return 0;
		}
		T v;
		std::memcpy(&v, &ram_[idx], sizeof(T));
		return v;
	}
	template <typename T> void Store(uint32_t addr, T v) {
		uint32_t idx;
		if (!Translate(addr, &idx) || idx + sizeof(T) > kRamSize) {
			fault_ = true;
			fault_addr_ = addr;
			return;
		}
		std::memcpy(&ram_[idx], &v, sizeof(T));
	}

	std::vector<uint8_t> ram_;
	mutable bool fault_ = false;
	mutable uint32_t fault_addr_ = 0;
};

// CPU state passed by pointer into the interpreter. Layout mirrors ARCHITECTURE.md section 4.
struct CpuState {
	uint32_t r[32] = {0};   // r[0] reads as 0; writes to r[0] are discarded by the interpreter.
	uint32_t hi = 0, lo = 0;
	// Exact here: the interpreter advances pc every instruction. This is the opposite of
	// the recompiled runtime, where CpuState::pc (src/rt/recomp.h) holds only the last
	// SR_YIELD preemption point. Do not carry an assumption about pc across the two.
	uint32_t pc = 0;
	union {
		float f[32];
		uint32_t fi[32];
	};
	uint32_t fcr31 = 0;
	// FP compare result. PPSSPP keeps the condition in a separate cached field (not in
	// fcr31 bit 23 on every compare), so c.cond.s does not show up as an fcr31 write in the
	// trace. The reference interpreter models it the same way and bc1t/bc1f read it.
	uint32_t fpcond = 0;

	// VFPU / COP0 to match recomp.h exactly to avoid struct parity drift
	union {
		float v[128];
		uint32_t vi[128];
	};
	uint32_t vfpuCtrl[16];
	uint32_t status;

	// Branch/delay-slot bookkeeping, matching the MIPS r4k model PPSSPP uses.
	uint32_t next_pc = 0;
	uint32_t in_delay_slot = 0;

	CpuState() {
		for (int i = 0; i < 32; i++) fi[i] = 0;
		for (int i = 0; i < 128; i++) vi[i] = 0;
		for (int i = 0; i < 16; i++) vfpuCtrl[i] = 0;
		status = 0;
	}
};

}  // namespace ref

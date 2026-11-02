// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

// C interface over ref::Run for the snapshot runner (see snapshot_ref.h).

#include "snapshot_ref.h"

#include "interp.h"

#include <cstring>
#include <new>

static_assert(sizeof(ref::CpuState) == NK_REF_CPUSTATE_SIZE, "reference CpuState size drift");
static_assert(ref::Memory::kRamBase == NK_REF_RAM_BASE, "reference RAM base drift");
static_assert(ref::Memory::kRamSize == NK_REF_RAM_SIZE, "reference RAM size drift");

struct NkRef {
	ref::Memory mem;
};

extern "C" NkRef *nk_ref_create(void) {
	return new (std::nothrow) NkRef();
}

extern "C" void nk_ref_destroy(NkRef *r) {
	delete r;
}

extern "C" uint8_t *nk_ref_ram(NkRef *r) {
	return r->mem.RamData();
}

extern "C" void nk_ref_run(NkRef *r, void *cpu, uint32_t entry_pc, uint32_t return_pc,
                           const uint32_t *import_stubs, uint32_t import_count,
                           uint64_t max_steps, NkRefResult *out) {
	ref::CpuState s;
	std::memcpy(&s, cpu, sizeof s);
	s.pc = entry_pc;
	std::memset(out, 0, sizeof *out);
	uint64_t executed = 0;
	for (;;) {
		// Between instructions, outside a delay slot, the next fetch decides whether the
		// call is over. ref::Run applies a taken transfer only after its delay slot, so
		// pc here is the architectural next instruction.
		if (!s.in_delay_slot) {
			if (s.pc == return_pc) {
				out->stop = NK_REF_RETURNED;
				out->pc = s.pc;
				break;
			}
			bool at_import = false;
			for (uint32_t i = 0; i < import_count; i++)
				if (import_stubs[i] == s.pc) at_import = true;
			if (at_import) {
				out->stop = NK_REF_IMPORT;
				out->pc = s.pc;
				break;
			}
		}
		if (executed >= max_steps) {
			out->stop = NK_REF_STEP_LIMIT;
			out->pc = s.pc;
			break;
		}
		ref::StepResult step = ref::Run(&s, &r->mem, 1, nullptr);
		executed += step.executed;
		if (step.reason == ref::StopReason::kStepLimit || step.reason == ref::StopReason::kRunning)
			continue;
		out->pc = step.pc;
		out->op = step.op;
		switch (step.reason) {
			case ref::StopReason::kSyscall: out->stop = NK_REF_SYSCALL; break;
			case ref::StopReason::kUnimplemented: out->stop = NK_REF_UNIMPLEMENTED; break;
			case ref::StopReason::kMemoryFault:
				out->stop = NK_REF_MEMORY_FAULT;
				out->fault_addr = r->mem.fault_addr();
				break;
			case ref::StopReason::kBreak: out->stop = NK_REF_BREAK; break;
			default: out->stop = NK_REF_UNIMPLEMENTED; break;
		}
		break;
	}
	out->executed = executed;
	std::memcpy(cpu, &s, sizeof s);
}

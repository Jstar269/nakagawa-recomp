// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)

// with the exact entry register state captured by the PPSSPP oracle (the "# init" line in
// the oracle trace). The emitted trace is then diffed against the oracle with
// tools/tracediff.py to locate the first divergence in the reference interpreter.
//
// Usage: run_elf <elf> <oracle-trace> <out-trace> [max_steps]
//
// This is a bring-up driver, not the loader of record (that is Phase 2). It handles only
// PT_LOAD of a non-relocated ELF, which is what the golden homebrew is.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "interp.h"

using namespace ref;

namespace {

std::vector<uint8_t> ReadFile(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	if (fseek(f, 0, SEEK_END) != 0) { fprintf(stderr, "cannot seek %s\n", path); fclose(f); exit(2); }
	long n = ftell(f);
	if (n < 0) { fprintf(stderr, "cannot size %s\n", path); fclose(f); exit(2); }
	if (fseek(f, 0, SEEK_SET) != 0) { fprintf(stderr, "cannot rewind %s\n", path); fclose(f); exit(2); }
	size_t size = static_cast<size_t>(n);
	if (static_cast<long>(size) != n) { fprintf(stderr, "file too large %s\n", path); fclose(f); exit(2); }
	std::vector<uint8_t> data(size);
	if (size > 0 && fread(data.data(), 1, size, f) != size) { fprintf(stderr, "short read %s\n", path); fclose(f); exit(2); }
	fclose(f);
	return data;
}

uint32_t Rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t Rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

bool RangeWithin(size_t total, uint64_t offset, uint64_t length) {
	return offset <= total && length <= static_cast<uint64_t>(total) - offset;
}

enum class GuestRangeStatus { kMapped, kUnmapped, kInvalid };

bool GuestRangeOverlapsModeledRam(uint32_t addr, uint32_t length) {
	if (length == 0) return false;
	uint64_t end = static_cast<uint64_t>(addr) + length;
	if (end > (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1)) return true;
	for (uint64_t alias = 0; alias <= 0xe0000000ULL; alias += 0x20000000ULL) {
		uint64_t base = alias + Memory::kRamBase;
		uint64_t limit = base + Memory::kRamSize;
		if (static_cast<uint64_t>(addr) < limit && end > base) return true;
	}
	return false;
}

GuestRangeStatus ClassifyGuestRange(Memory *mem, uint32_t addr, uint32_t length, uint32_t *index) {
	if (!mem || !index) return GuestRangeStatus::kInvalid;
	if (length == 0) {
		*index = 0;
		return GuestRangeStatus::kUnmapped;
	}
	uint64_t last64 = static_cast<uint64_t>(addr) + length - 1;
	if (last64 > std::numeric_limits<uint32_t>::max()) return GuestRangeStatus::kInvalid;
	uint32_t first_index = 0, last_index = 0;
	bool first_mapped = mem->Translate(addr, &first_index);
	bool last_mapped = mem->Translate(static_cast<uint32_t>(last64), &last_index);
	if (!first_mapped && !last_mapped)
		return GuestRangeOverlapsModeledRam(addr, length) ? GuestRangeStatus::kInvalid : GuestRangeStatus::kUnmapped;
	if (!first_mapped || !last_mapped) return GuestRangeStatus::kInvalid;
	if (last_index < first_index) return GuestRangeStatus::kInvalid;
	if (static_cast<uint64_t>(first_index) + length > Memory::kRamSize) return GuestRangeStatus::kInvalid;
	*index = first_index;
	return GuestRangeStatus::kMapped;
}

[[noreturn]] void ElfError(const char *message) {
	fprintf(stderr, "%s\n", message);
	exit(2);
}

// Load PT_LOAD segments into guest memory. Returns the ELF entry point.
uint32_t LoadElf(const std::vector<uint8_t> &elf, Memory *mem) {
	if (!mem) ElfError("null memory");
	if (elf.size() < 52 || memcmp(elf.data(), "\x7f""ELF", 4) != 0) ElfError("not an ELF");
	uint32_t e_entry = Rd32(&elf[24]);
	uint32_t e_phoff = Rd32(&elf[28]);
	uint16_t e_phentsize = Rd16(&elf[42]);
	uint16_t e_phnum = Rd16(&elf[44]);
	if (e_phnum != 0 && e_phentsize < 32) ElfError("ELF program header entry too small");
	uint64_t ph_table_size = static_cast<uint64_t>(e_phentsize) * e_phnum;
	if (!RangeWithin(elf.size(), e_phoff, ph_table_size)) ElfError("ELF program header table out of range");
	for (uint16_t i = 0; i < e_phnum; i++) {
		uint64_t ph_offset = static_cast<uint64_t>(e_phoff) + static_cast<uint64_t>(i) * e_phentsize;
		if (!RangeWithin(elf.size(), ph_offset, 32)) ElfError("ELF program header out of range");
		const uint8_t *ph = elf.data() + static_cast<size_t>(ph_offset);
		uint32_t p_type = Rd32(ph + 0);
		uint32_t p_offset = Rd32(ph + 4);
		uint32_t p_vaddr = Rd32(ph + 8);
		uint32_t p_filesz = Rd32(ph + 16);
		uint32_t p_memsz = Rd32(ph + 20);
		if (p_type != 1)  // PT_LOAD
			continue;
		if (p_filesz > p_memsz) ElfError("ELF PT_LOAD filesz exceeds memsz");
		if (!RangeWithin(elf.size(), p_offset, p_filesz)) ElfError("ELF PT_LOAD data out of range");
		uint32_t guest_index = 0;
		GuestRangeStatus guest_range = ClassifyGuestRange(mem, p_vaddr, p_memsz, &guest_index);
		if (guest_range == GuestRangeStatus::kInvalid) ElfError("ELF PT_LOAD guest range crosses modeled memory");
		if (guest_range == GuestRangeStatus::kUnmapped) continue;
		if (p_filesz != 0)
			memcpy(mem->RamData() + guest_index, elf.data() + p_offset, p_filesz);
		if (p_memsz > p_filesz)
			memset(mem->RamData() + guest_index + p_filesz, 0, p_memsz - p_filesz);
	}
	return e_entry;
}

bool ParseIndexedRegister(const char *name, char prefix, uint32_t *index) {
	if (!name || !index || name[0] != prefix) return false;
	if (name[1] < '0' || name[1] > '9') return false;
	char *end = nullptr;
	errno = 0;
	unsigned long long value = strtoull(name + 1, &end, 10);
	if (errno != 0 || !end || end == name + 1 || *end != '\0' || value > 31ULL) return false;
	*index = static_cast<uint32_t>(value);
	return true;
}

bool ParseHex32(const char *text, uint32_t *value) {
	if (!text || !value || *text == '\0' || *text == '-' || *text == '+' || *text == ' ' || *text == '\t') return false;
	const char *start = text;
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		start += 2;
	}
	if (!((*start >= '0' && *start <= '9') || (*start >= 'a' && *start <= 'f') || (*start >= 'A' && *start <= 'F'))) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	unsigned long long parsed = strtoull(start, &end, 16);
	if (errno != 0 || !end || end == start || *end != '\0' || parsed > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) return false;
	*value = static_cast<uint32_t>(parsed);
	return true;
}

// Parse the "# init r0=.. ... fcr31=.. f0=.. .." line from the oracle trace into a CpuState.
bool SeedFromInit(const char *trace_path, CpuState *s) {
	FILE *f = fopen(trace_path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
	char buf[8192];
	bool found = false;
	while (fgets(buf, sizeof(buf), f)) {
		if (strncmp(buf, "# init", 6) != 0)
			continue;
		found = true;
		char *tok = strtok(buf + 6, " \t\n");
		while (tok) {
			char *eq = strchr(tok, '=');
			if (eq) {
				*eq = 0;
				uint32_t val = 0, index = 0;
				const char *name = tok;
				if (ParseHex32(eq + 1, &val)) {
					if (ParseIndexedRegister(name, 'r', &index)) s->r[index] = val;
					else if (ParseIndexedRegister(name, 'f', &index)) s->fi[index] = val;
					else if (strcmp(name, "hi") == 0) s->hi = val;
					else if (strcmp(name, "lo") == 0) s->lo = val;
					else if (strcmp(name, "fcr31") == 0) s->fcr31 = val;
				}
			}
			tok = strtok(nullptr, " \t\n");
		}
		break;
	}
	fclose(f);
	return found;
}

const char *ReasonName(StopReason r) {
	switch (r) {
		case StopReason::kRunning: return "running";
		case StopReason::kSyscall: return "syscall";
		case StopReason::kUnimplemented: return "unimplemented";
		case StopReason::kMemoryFault: return "memory-fault";
		case StopReason::kBreak: return "break";
		case StopReason::kStepLimit: return "step-limit";
	}
	return "?";
}

}  // namespace

#ifndef SR_SELFTEST_ONLY
int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: run_elf <elf> <oracle-trace> <out-trace> [max_steps]\n");
		return 2;
	}
	const char *elf_path = argv[1];
	const char *oracle_trace = argv[2];
	const char *out_path = argv[3];
	unsigned long long max_steps = argc > 4 ? strtoull(argv[4], nullptr, 10) : 2000000ULL;

	Memory mem;
	std::vector<uint8_t> elf = ReadFile(elf_path);
	uint32_t entry = LoadElf(elf, &mem);

	CpuState s;
	if (!SeedFromInit(oracle_trace, &s)) {
		fprintf(stderr, "no '# init' line in %s; rebuild oracle trace with init dump\n", oracle_trace);
		return 2;
	}
	s.pc = entry;

	FILE *out = fopen(out_path, "wb");
	if (!out) { fprintf(stderr, "cannot open %s for write\n", out_path); return 2; }
	TraceSink sink(out);
	sink.Header("hello", entry);
	sink.InitDump(&s);

	StepResult res = Run(&s, &mem, max_steps, &sink);
	fclose(out);

	const char *ppm_path = getenv("SR_PPM_DUMP");
	if (ppm_path) {
		uint32_t fb_addr = 0x09000000;
		FILE *pf = fopen(ppm_path, "wb");
		if (pf) {
			fprintf(pf, "P6\n64 64\n255\n");
			for (int i = 0; i < 64 * 64; i++) {
				uint32_t pixel = mem.Read32(fb_addr + i * 4);
				uint8_t r = pixel & 0xFF;
				uint8_t g = (pixel >> 8) & 0xFF;
				uint8_t b = (pixel >> 16) & 0xFF;
				fputc(r, pf);
				fputc(g, pf);
				fputc(b, pf);
			}
			fclose(pf);
			fprintf(stderr, "[run_elf] Framebuffer PPM dumped to %s\n", ppm_path);
		}
	}

	fprintf(stderr, "stopped: reason=%s pc=0x%08x op=0x%08x\n", ReasonName(res.reason), res.pc, res.op);

	return 0;
}
#endif

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: the VFPU fuzzers, verify, run_elf and the shader checks.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# vfpu_fuzz — build and run the VFPU differential fuzzer (generated codegen vs sr_vfpu_interp).
# Generates the per-game cases header from the ELF, then links a standalone harness that drives
# every distinct VFPU compute word through both paths on randomized register states. Depends on
# the runtime objects (not the `compile` link of hst.exe) because the generated bodies call
# sr_vread/sr_vwrite/sr_vfpu_* which live in recomp.c. driver.o is excluded since it owns the
# real program's main(); ge.o (RT_GE_O) is linked in because recomp.o references it.
VFPU_FUZZ_H := $(BUILD_DIR)/vfpu_fuzz_cases.h
VFPU_FUZZ_PREGENERATED ?= 0
ifeq ($(VFPU_FUZZ_PREGENERATED),1)
VFPU_FUZZ_TITLE_OBJ :=
VFPU_FUZZ_CHUNK_OBJS :=
VFPU_FUZZ_VALIDATE := vfpu_fuzz_validate_synthetic
# CI/public mode: the caller generated a synthetic cases header explicitly.  Do not
# introduce a fake GAME_ELF dependency or regenerate from proprietary/private input.
$(VFPU_FUZZ_H):
	"$(PYTHON)" tools/vfpu_fuzz_gen.py --require-synthetic "$@"
else
VFPU_FUZZ_TITLE_OBJ := $(BUILD_DIR)/$(GAME_NAME)_recomp.o
VFPU_FUZZ_CHUNK_OBJS := $(CHUNK_OBJS)
VFPU_FUZZ_VALIDATE :=
$(VFPU_FUZZ_H): $(GAME_INPUT_PREREQ) tools/vfpu_fuzz_gen.py tools/analyze.py tools/analyze_cfg.py tools/codegen.py
	"$(PYTHON)" tools/vfpu_fuzz_gen.py --env-elf $(VFPU_FUZZ_H) --base=$(GAME_BASE) $(EXTRA_SPAN_ARG)
endif

vfpu_fuzz_validate_synthetic:
	"$(PYTHON)" tools/vfpu_fuzz_gen.py --require-synthetic "$(VFPU_FUZZ_H)"

$(BUILD_DIR)/vfpu_fuzz.o: src/rt/vfpu_fuzz.c $(VFPU_FUZZ_H) src/rt/recomp.h
	$(CC) -O0 -fno-strict-aliasing -Isrc/rt -I$(BUILD_DIR) -DSR_SDL3VK $(DEPFLAGS) -c src/rt/vfpu_fuzz.c -o $@

# Two-phase build (same rationale as `all`): `pipeline` must generate the chunk
# .c files before this target is parsed, or CHUNK_OBJS ($(wildcard)) resolves empty
# and the chunk objects are never compiled. Run codegen first, then build/link in a
# second make pass so the chunk objects are discovered.
ifeq ($(VFPU_FUZZ_PREGENERATED),1)
vfpu_fuzz:
	$(MAKE) VFPU_FUZZ_PREGENERATED=1 vfpu_fuzz_build
else
vfpu_fuzz:
	$(MAKE) pipeline
	$(MAKE) vfpu_fuzz_build
endif

# RT_OBJS carries hle.o, which calls into the ATRAC decode bridges, so
# every target that links RT_OBJS must also link the bridge and the imported
# decoder TUs -- exactly as `compile` and `runtime-objects` do. Omitting them
# here is an undefined-reference link failure, not a smaller binary.
vfpu_fuzz_build: $(VFPU_FUZZ_H) $(VFPU_FUZZ_VALIDATE) $(BUILD_DIR)/vfpu_fuzz.o $(VFPU_FUZZ_CHUNK_OBJS) $(RT_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(VFPU_FUZZ_TITLE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_fuzz.exe \
		$(BUILD_DIR)/vfpu_fuzz.o \
		$(VFPU_FUZZ_TITLE_OBJ) \
		$(VFPU_FUZZ_CHUNK_OBJS) \
		$(RT_GE_O) \
		$(filter-out $(BUILD_DIR)/driver.o,$(RT_OBJS)) \
		$(ATRAC3P_OBJS) \
		$(BUILD_DIR)/atrac3p_bridge.o \
		$(LIBS)
	$(BUILD_DIR)/vfpu_fuzz.exe

# psp-oracle-vfpu -- Nakagawa side of the VFPU transcendental hardware oracle
# (Loop A of docs/HARDWARE_ORACLE.md).  Deliberately game-independent: it drives the
# production sr_vfpu_* implementations in recomp.c over the shared input vector in
# fixtures/vfpu_oracle/vfpu_oracle_cases.h and emits the same record shape the PSP
# probe emits.  No GAME_ELF and no generated chunks are required; assets/vfpu/ is the
# only data input (override the directory with PSP_VFPU_TABLES).
PSP_VFPU_ORACLE_EXE := $(BUILD_DIR)/vfpu_oracle_host.exe

$(PSP_VFPU_ORACLE_EXE): src/rt/vfpu_oracle_host.c fixtures/vfpu_oracle/vfpu_oracle_cases.h $(RT_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o
	$(CC) $(CFLAGS) -Isrc/rt -Ifixtures/vfpu_oracle $(LDFLAGS) -o $@ 		src/rt/vfpu_oracle_host.c 		$(RT_GE_O) 		$(filter-out $(BUILD_DIR)/driver.o,$(RT_OBJS)) 		$(ATRAC3P_OBJS) 		$(BUILD_DIR)/atrac3p_bridge.o 		$(LIBS)

psp-oracle-vfpu-build: $(PSP_VFPU_ORACLE_EXE)

# Emit the Nakagawa stream.  All four provenance flags are required by the harness;
# unmeasured provenance must fail the acceptance gate rather than default to a
# placeholder.  Output goes to an ignored path.
PSP_VFPU_ORACLE_OUT ?= $(BUILD_DIR)/vfpu_oracle/nakagawa.stdout.txt
PSP_VFPU_ORACLE_MODEL ?= unknown
PSP_VFPU_ORACLE_FIRMWARE ?= unknown
PSP_VFPU_ORACLE_COMMIT ?= $(NK_GIT_REV_HEAD)

psp-oracle-vfpu: $(PSP_VFPU_ORACLE_EXE)
	"$(PYTHON)" -c "from pathlib import Path; Path(r'$(BUILD_DIR)/vfpu_oracle').mkdir(parents=True, exist_ok=True)"
	"$(PYTHON)" tools/psp_oracle/run_nakagawa_vfpu.py --executable "$(PSP_VFPU_ORACLE_EXE)" --output "$(PSP_VFPU_ORACLE_OUT)" -- --model "$(PSP_VFPU_ORACLE_MODEL)" --firmware "$(PSP_VFPU_ORACLE_FIRMWARE)" --source-commit "$(PSP_VFPU_ORACLE_COMMIT)"

# verify — differential smoke gates (no recompiler build needed; runs Python analysis tools
# and the host reference interpreter). Codegen and microtest compare project output against
# their supplied traces. The hardware gate separately requires a matching v2 PSP_HARDWARE
# and LOCAL_COSIM pair; PPSSPP_CORROBORATIVE and v1 traces cannot satisfy it.
#
# Correct gate signatures (see tools/codegen_gate.py and tools/microtest_gate.py):
#   codegen_gate.py   <elf> <oracle.trace> <workdir>
#   microtest_gate.py <run_elf.exe> <module.elf> <oracle.trace> <workdir>
#
# Usage: make verify GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 \
#          CODEGEN_ORACLE=oracle/eboot.trace \
#          MICROTEST_MODULE=build/hst/microtest.elf MICROTEST_ORACLE=oracle/microtest.trace \
#          PSP_HARDWARE_TRACE=oracle/psp-hardware.trace LOCAL_COSIM_TRACE=oracle/local-cosim.trace
verify: run_elf
	"$(PYTHON)" tools/verify_gates.py --cc "$(CC)" --env-elf \
		--run-elf "$(RUN_ELF_EXE)" --workdir "$(VERIFY_WORKDIR)" \
		--codegen-oracle "$(CODEGEN_ORACLE)" --microtest-module "$(MICROTEST_MODULE)" \
		--microtest-oracle "$(MICROTEST_ORACLE)" \
		--psp-hardware-trace "$(PSP_HARDWARE_TRACE)" --local-cosim-trace "$(LOCAL_COSIM_TRACE)"

# run_elf — host reference-interpreter driver used by microtest_gate. Built WITHOUT
# -DSR_SELFTEST_ONLY so run_elf.cpp's main() (ELF loader + trace driver) is included.
run_elf:
	$(CXX) -std=c++17 -O1 -Isrc/ref -Isrc/rt -o $(RUN_ELF_EXE) \
		src/ref/run_elf.cpp src/ref/interp.cpp -fno-exceptions

run: all
	./$(BUILD_DIR)/$(GAME_NAME).exe --image $(BUILD_DIR)/$(GAME_NAME)_image.bin $(GAME_BASE) $(GAME_ENTRY) none none --gui

# shaders — deterministic SPIR-V regeneration for the SDL3/Vulkan GPU backend.
# Normal builds verify source/embedding/manifest hashes without requiring glslc. The
# stricter shader-repro-verify target recompiles with glslc and compares byte-for-byte.
SHADER_DIR     := src/rt/gpu_sdl3vk
VERT_SHADER    := $(SHADER_DIR)/shaders/psp.vert
FRAG_SHADER    := $(SHADER_DIR)/shaders/psp.frag
VERT_SPV       := $(SHADER_DIR)/psp_vert.spv
FRAG_SPV       := $(SHADER_DIR)/psp_frag.spv
VERT_INC       := $(SHADER_DIR)/psp_vert.inc
FRAG_INC       := $(SHADER_DIR)/psp_frag.inc

shaders:
	"$(PYTHON)" tools/shader_embed.py regenerate --glslc "$(GLSLC)"

shader-verify:
	"$(PYTHON)" tools/shader_embed.py verify

shader-repro-verify:
	"$(PYTHON)" tools/shader_embed.py verify --recompile --glslc "$(GLSLC)"

# -----------------------------------------------------------------------------
# Native Product Core Tests
# -----------------------------------------------------------------------------
# Binaries that share a compile flag set link one set of compiled core objects instead of
# recompiling the product core sources for every binary (#735 item 3). Each set is built
# with the exact flag list its binaries used before, so the objects are the same ones. The
# set targets are phony: every make invocation compiles each set once, as the per-binary
# compiles did, and file timestamps never decide freshness. The test binaries below are
# built by macros rather than by recursive $(MAKE) calls, so native-core-tests parses this
# Makefile once instead of once per sub-make (#735 item 7).
NC_CFLAGS_CORE   := -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/rt
NC_CFLAGS_PLAYER := $(NC_CFLAGS_CORE) -Isrc/player
NC_SOURCES       := $(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES)
NC_OBJS_CORE     := $(addprefix $(NC_OBJ_DIR)/core/,$(notdir $(NC_SOURCES:.c=.o)))
NC_OBJS_PLAYER   := $(addprefix $(NC_OBJ_DIR)/player/,$(notdir $(NC_SOURCES:.c=.o)))

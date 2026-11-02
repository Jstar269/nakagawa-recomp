# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: runtime title configuration, the profile stamps and the hardware gate inputs.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# Runtime title configuration. The compiled runtime carries no title identity of its
# own: tools/title_runtime_config.py turns a *validated* title manifest's optional
# runtime_bindings block into a build-local header that only src/rt/title_config.c
# includes. With TITLE_MANIFEST empty the generator emits the generic configuration in
# which every optional binding is disabled -- so `make runtime-objects` needs no game
# input at all, and no default build inherits any title's addresses.
#
# HST binds its real values through its title manifest
# (assets/titles/hst-ucus98701.json, supplied by nk_manager.ps1 -TitleManifest or by
# TITLE_MANIFEST= on a direct Make command line). They are deliberately not encoded here.
TITLE_MANIFEST ?=
TITLE_CONFIG_TOOL := tools/title_runtime_config.py
TITLE_CONFIG_DIR ?= $(BUILD_DIR)
TITLE_CONFIG_HEADER := $(TITLE_CONFIG_DIR)/sr_title_config.h
TITLE_CONFIG_ARG := $(if $(strip $(TITLE_MANIFEST)),--manifest $(strip $(TITLE_MANIFEST)),)
# Identity of the effective configuration. Bound into RUNTIME_PROFILE_HASH below so a
# changed title binding invalidates stale runtime objects instead of relinking silently.
ifdef NK_INFO_ONLY
TITLE_CONFIG_DIGEST := info-only
else
TITLE_CONFIG_DIGEST := $(shell "$(PYTHON)" $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --print-digest)
endif
# An unreadable or invalid manifest prints nothing. Refusing here keeps a rejected title
# configuration from becoming an empty profile entry that hashes like some other build.
ifeq ($(strip $(TITLE_CONFIG_DIGEST)),)
$(error title runtime configuration could not be resolved; run "$(PYTHON) $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --print-digest" for the reason)
endif

# A build that explicitly identifies itself as HST must say where HST's title
# configuration comes from. Without it every optional binding is disabled and the build
# would quietly produce an HST executable with no fallback entry, no worker/launcher role
# and no VBLANK counters -- a broken runtime that looks like a successful build. This is
# a build-time refusal, not a title default: generic builds are untouched, and nothing
# here makes `runtime-objects` require a retail or title input.
ifeq ($(GAME_NAME),hst)
ifeq ($(strip $(TITLE_MANIFEST)),)
TITLE_CONFIG_HST_UNBOUND := 1
endif
endif

# Content-addressed identity of the EFFECTIVE title configuration, and the stamp that
# carries it. This is what the generated header depends on, because none of the header's
# natural prerequisites can express "the configuration changed":
#
#   - The manifest FILE is not a stable prerequisite. Dropping TITLE_MANIFEST removes it
#     from the prerequisite list entirely, so a bound -> unbound transition presents Make
#     with a target that is newer than everything left, the recipe does not run, and the
#     refusal below -- a recipe line -- never fires. The build then compiles a fresh
#     title_config.o against the PREVIOUS title's header while RUNTIME_PROFILE_HASH
#     records the generic digest.
#   - mtime cannot express it in the other direction either: a manifest older than an
#     existing generic header leaves that header "up to date", so the profile records the
#     title digest while the compiled configuration binds nothing.
#
# TITLE_CONFIG_DIGEST already covers the source id and every binding, so it subsumes the
# manifest file's content; the HST-unbound state is appended because it shares the generic
# digest yet must never reuse a header generated for some other configuration.
#
# The manifest file is deliberately NOT also a prerequisite. The digest is derived from
# its content at parse time, so the file adds nothing the identity does not already carry
# -- while touching it without changing it would make this recipe run on every build (the
# generator writes only on change, so the header's mtime would never catch up).
#
# The stamp DELETES the header rather than relying on being newer than it, and is included
# below with the other profile stamps so Make restarts and sees that deletion before it
# judges freshness. A changed identity and a regenerated header can land inside one
# filesystem timestamp tick, and "newer" cannot decide that; "absent" always can.
TITLE_CONFIG_IDENTITY := $(TITLE_CONFIG_DIGEST)$(if $(TITLE_CONFIG_HST_UNBOUND),-hst-unbound,)
TITLE_CONFIG_STAMP := $(TITLE_CONFIG_DIR)/.title-config-$(TITLE_CONFIG_IDENTITY)
$(TITLE_CONFIG_STAMP): $(BUILD_PROFILE_TOOL)
	"$(PYTHON)" $(BUILD_PROFILE_TOOL) stamp --output "$@" --stale-glob ".title-config-*" --value "$(TITLE_CONFIG_IDENTITY)" --invalidate "$(TITLE_CONFIG_HEADER)"

$(TITLE_CONFIG_HEADER): $(TITLE_CONFIG_TOOL) tools/title_manifest.py $(TITLE_CONFIG_STAMP)
ifeq ($(TITLE_CONFIG_HST_UNBOUND),1)
	$(error GAME_NAME=hst needs a validated title manifest: pass TITLE_MANIFEST=<path> or use nk_manager.ps1 -TitleManifest. Direct Make does not infer HST defaults; generic builds need no manifest when they use a different GAME_NAME.)
endif
	"$(PYTHON)" $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --output $@

# Title-neutral configuration for the game-input-free selftests. Those targets assert
# generic PSP behavior and install their own role fixtures, so binding them to a title
# would make their result depend on which title the tree happens to be building.
GENERIC_TITLE_CONFIG_DIR := $(BUILD_DIR)/title-config/generic
GENERIC_TITLE_CONFIG_HEADER := $(GENERIC_TITLE_CONFIG_DIR)/sr_title_config.h

$(GENERIC_TITLE_CONFIG_HEADER): $(TITLE_CONFIG_TOOL) tools/title_manifest.py
	"$(PYTHON)" $(TITLE_CONFIG_TOOL) --output $@

RUNTIME_PROFILE_MANIFEST := $(BUILD_DIR)/runtime_profile.json
RECOMP_PROFILE_MANIFEST := $(BUILD_DIR)/recomp_profile.json
CODEGEN_PROFILE_MANIFEST := $(BUILD_DIR)/codegen_profile.json

# Verification-gate inputs. Codegen/microtest oracle traces remain external inputs; v1
# traces are accepted for those non-hardware comparisons and reported as corroborative.
# The hardware gate requires a strict v2 PSP_HARDWARE + LOCAL_COSIM pair.
#   make verify GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 \
#     CODEGEN_ORACLE=oracle/eboot.trace \
#     MICROTEST_MODULE=build/hst/microtest.elf MICROTEST_ORACLE=oracle/microtest.trace \
#     PSP_HARDWARE_TRACE=oracle/psp-hardware.trace LOCAL_COSIM_TRACE=oracle/local-cosim.trace
# When a codegen/microtest input is absent that gate reports NOT_RUN with a non-zero signal.
# The hardware pair is optional: with neither trace set it reports NOT_RUN without failing
# the target, so `make verify` can still succeed; setting only one of the two fails.
CODEGEN_ORACLE   ?=
MICROTEST_MODULE ?=
MICROTEST_ORACLE ?=
PSP_HARDWARE_TRACE ?=
LOCAL_COSIM_TRACE ?=
RUN_ELF_EXE      ?= $(BUILD_DIR)/run_elf.exe
VERIFY_WORKDIR   ?= $(BUILD_DIR)/verify

CXX        ?= g++

# ATRAC3/ATRAC3+ decoder import sources. Shared by the runtime link and the
# standalone selftest targets; both codec paths use the n4.4 ATRAC common code.
ATRAC3P_SRCS := src/rt/atrac3p/atrac3p_api.c \
	src/rt/atrac3p/libavcodec/atrac.c \
	src/rt/atrac3p/libavcodec/atrac3.c \
	src/rt/atrac3p/libavcodec/atrac3plus.c \
	src/rt/atrac3p/libavcodec/atrac3plusdec.c \
	src/rt/atrac3p/libavcodec/atrac3plusdsp.c \
	src/rt/atrac3p/libavcodec/bitstream.c \
	src/rt/atrac3p/libavcodec/fft_float.c \
	src/rt/atrac3p/libavcodec/fft_init_table.c \
	src/rt/atrac3p/libavcodec/mdct_float.c \
	src/rt/atrac3p/libavcodec/sinewin.c \
	src/rt/atrac3p/libavutil/float_dsp.c \
	src/rt/atrac3p/libavutil/intmath.c \
	src/rt/atrac3p/libavutil/log2_tab.c \
	src/rt/atrac3p/libavutil/mem.c \
	src/rt/atrac3p/libavutil/reverse.c


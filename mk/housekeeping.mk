# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: profile stamps, project fonts, compile and the clean targets.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# Treat profile stamps as generated included makefiles. GNU Make restarts after
# creating a missing flavour, so objects invalidated by that recipe are absent
# before target freshness is evaluated (avoiding timestamp-resolution races).
ifeq ($(strip $(filter clean-preview,$(MAKECMDGOALS))),)
ifeq ($(strip $(filter clean distclean,$(MAKECMDGOALS))$(NK_INFO_ONLY)),)
-include $(CODEGEN_PROFILE_STAMP) $(RUNTIME_PROFILE_STAMP) $(RECOMP_PROFILE_STAMP) $(TITLE_CONFIG_STAMP)
endif
endif
-include $(DEP_FILES)

# The project PGFs depend only on their pinned sources and the converter, not on the title
# or build configuration, so every BUILD_DIR under one BUILD_ROOT shares one generated set
# (the platform ladder's rungs and each title build would otherwise regenerate them).
PROJECT_FONT_GENERATED_DIR := $(BUILD_ROOT)/project-font
PROJECT_FONT_GENERATED_DIR_PY := $(subst \,/,$(PROJECT_FONT_GENERATED_DIR))

# Keep every internal target visible to the static target inventory.
project-japanese-font:
project-latin-font:
project-korean-font:

# All three project slots use this rule family. Each source pin, repertoire, and
# project-owned PGF name is data; output staging is handled once by tools/copy_build_assets.ps1.
define PROJECT_FONT_RULE
project-$(1)-font: $(PROJECT_FONT_GENERATED_DIR)/$(2)

$(PROJECT_FONT_GENERATED_DIR)/$(2): $(3) $(4) $(5) tools/ttf2pgf.py tools/pgf_writer.py docs/cleanroom/PGF_CONVERTER_SPEC.md
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(PROJECT_FONT_GENERATED_DIR_PY)').mkdir(parents=True, exist_ok=True)"
	"$(PYTHON)" tools/ttf2pgf.py "$$@" "$(3)" --pin "$(4)" $(6) $(8) --font-name "$(7)" --font-type "Regular" --manifest "$(PROJECT_FONT_GENERATED_DIR_PY)/$(basename $(2)).json"
	"$(PYTHON)" -c "import shutil; shutil.copyfile('$(5)', '$(PROJECT_FONT_GENERATED_DIR_PY)/$(basename $(2))-OFL.txt')"
endef

$(eval $(call PROJECT_FONT_RULE,japanese,nkjpn.pgf,fixtures/fonts/mplus1p/MPLUS1p-Regular.ttf,fixtures/fonts/mplus1p/PIN.json,fixtures/fonts/mplus1p/OFL.txt,--jis-x-0208-japanese,Nakagawa Open Japanese,--ppem 12))
$(eval $(call PROJECT_FONT_RULE,latin,nkltn.pgf,fixtures/fonts/mplus1p/MPLUS1p-Regular.ttf,fixtures/fonts/mplus1p/PIN.json,fixtures/fonts/mplus1p/OFL.txt,--latin-1-text,Nakagawa Open Latin,--ppem 13 --embolden-x 6 --embolden-y 4 --embolden-advance 3))
$(eval $(call PROJECT_FONT_RULE,korean,nkkr.pgf,fixtures/fonts/gowundodum/GowunDodum-Regular.ttf,fixtures/fonts/gowundodum/PIN.json,fixtures/fonts/gowundodum/OFL.txt,--ks-x-1001-korean,Nakagawa Open Korean,--ppem 12))

project-fonts: $(PROJECT_FONT_SLOT_TARGETS)

# Project PGFs are generated before compile but are not link inputs, so keep the
# dependency order-only and preserve the link set below.
compile: | project-fonts

# The generated headers a compile reads, built on their own first so
# tools/title_codegen_plan.py can key every object before Make compiles it (#1306).
compile-inputs: $(TITLE_CONFIG_HEADER)
compile: shader-verify $(CHUNK_OBJS) $(LIBRARY_OBJS) $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(TITLE_ICON_OBJ) | sdl3-check
	$(file >$(LINK_OBJECTS_RSP),$(subst \,/,$(LINK_OBJECTS)))
	$(CC) $(CFLAGS) $(LDFLAGS) $(LINK_MAP_ARG) $(REPRODUCIBLE_LINK_FLAG) -o $(BUILD_DIR)/$(GAME_NAME)$(EXE_EXT) \
		@$(LINK_OBJECTS_RSP) \
		$(LIBS)
	$(ASSET_COPY_STEP)
	@"$(PYTHON)" -c "print('Build finished: $(BUILD_DIR)/$(GAME_NAME)$(EXE_EXT)')"

clean:
	"$(PYTHON)" -c "import shutil, sys; from pathlib import Path; p = Path(r'$(BUILD_DIR)'); [shutil.rmtree(p) if p.is_dir() else p.unlink()] if p.exists() else None"

# The shared fixture trees clean-fixtures removes, all beneath BUILD_ROOT.
CLEAN_FIXTURE_DIRS := $(PRODUCTION_SMOKE_DIR) $(PRODUCTION_SMOKE_GAP_DIR) $(COSIM_DIR) \
	$(addprefix $(BUILD_ROOT)/,nakagawa_psp_oracle vfpu_oracle portable-core verify link)

# The ephemeral build and run logs distclean and clean-all remove from LOG_DIR (the set
# nk_manager.ps1 writes there). Every other file under LOG_DIR is preserved.
EPHEMERAL_LOG_NAMES := build_out_recomp.log build_err_recomp.log recomp_err.log obj_err.log \
	link_err.log stdout_run.log stderr_run.log
REMOVE_EPHEMERAL_LOGS = "$(PYTHON)" -c "import sys; from pathlib import Path; [Path(sys.argv[1], name).unlink(missing_ok=True) for name in sys.argv[2:]]" "$(LOG_DIR)" $(EPHEMERAL_LOG_NAMES)

clean-fixtures:
	"$(PYTHON)" -c "import shutil, sys; from pathlib import Path; [shutil.rmtree(p) if p.is_dir() else p.unlink() for p in map(Path, sys.argv[1:]) if p.exists()]" $(foreach d,$(CLEAN_FIXTURE_DIRS),"$(d)")

tidy: distclean

distclean:
	@echo "Removing stale build artefacts (preserving .exe and .pdb for debugger)"
	"$(PYTHON)" -c "from pathlib import Path; r=Path(r'$(BUILD_DIR)'); [p.unlink(missing_ok=True) for g in ('**/*.o','**/*.d','.*-profile-*','*_profile.json') for p in r.glob(g)] if r.exists() else None"
	$(REMOVE_EPHEMERAL_LOGS)

clean-all: clean clean-fixtures
	"$(PYTHON)" -c "import shutil, sys; from pathlib import Path; b = Path(sys.argv[1]); [shutil.rmtree(p) if p.is_dir() else p.unlink() for p in b.iterdir()] if b.exists() else None" "$(BUILD_ROOT)"
	$(REMOVE_EPHEMERAL_LOGS)

# Preview-first workspace clean (issue #368): plans allowlisted output roots and
# prints the exact file set. Deletion requires CONFIRM=1 (which passes --yes);
# OLDER_THAN=<days> bounds the plan by file age. Existing clean targets are untouched.
clean-preview:
	"$(PYTHON)" tools/nk_clean.py $(if $(filter 1,$(CONFIRM)),--yes,) $(if $(strip $(OLDER_THAN)),--older-than $(OLDER_THAN),)

# sched-selftest — white-box scheduler/lifecycle unit tests (tests/native/sched_selftest.c).
# No game inputs needed; #includes sched.c for direct access to pick_next()/TCB state and
# links the real sr_coro backend. Asserts PSP strict-priority selection, equal-priority
# rotation, non-runnable exclusion, implicit thread exit on entry return, role-UID
# capture, stack-exhaustion create failure, and sr_coro self-switch/park guards.
# Exit code 0 = all invariants hold.
# Title-configuration matrix. The SAME scheduler source is built three times against
# three generated runtime title configurations, so a title binding that leaked back into
# generic scheduler code fails at least one of them:
#   generic  -- no title configuration; every optional binding disabled
#   fixture-a -- assets/titles/pspdev-phase5.json (source-owned synthetic addresses)
#   fixture-b -- assets/titles/synthetic.json (a different source-owned address set)
# Each generated header lands in its own directory so the three builds cannot share one.
SCHED_SELFTEST_CONFIGS := generic fixture-a fixture-b
SCHED_SELFTEST_MANIFEST_generic :=
SCHED_SELFTEST_MANIFEST_fixture-a := assets/titles/pspdev-phase5.json
SCHED_SELFTEST_MANIFEST_fixture-b := assets/titles/synthetic.json

sched-selftest:
	@$(foreach cfg,$(SCHED_SELFTEST_CONFIGS),$(MAKE) --no-print-directory sched-selftest-one SCHED_SELFTEST_CONFIG=$(cfg)$(NEWLINE)	)

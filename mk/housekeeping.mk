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
project-latin-small-font:
project-korean-font:
project-latin-1-font:
project-latin-2-font:
project-latin-3-font:
project-latin-4-font:
project-latin-5-font:
project-latin-6-font:
project-latin-7-font:
project-latin-9-font:
project-latin-10-font:
project-latin-11-font:
project-latin-12-font:
project-latin-13-font:
project-latin-14-font:
project-latin-15-font:

# The project fonts use this rule family. Each source pin, repertoire, project-owned PGF name and
# style is data; output staging is handled once by tools/copy_build_assets.ps1.
# Arguments: 1 target stem, 2 output, 3 TTF, 4 pin, 5 licence text, 6 repertoire, 7 PGF name,
# 8 converter settings, 9 PGF style.
define PROJECT_FONT_RULE
project-$(1)-font: $(PROJECT_FONT_GENERATED_DIR)/$(2)

$(PROJECT_FONT_GENERATED_DIR)/$(2): $(3) $(4) $(5) tools/ttf2pgf.py tools/pgf_writer.py docs/cleanroom/PGF_CONVERTER_SPEC.md
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(PROJECT_FONT_GENERATED_DIR_PY)').mkdir(parents=True, exist_ok=True)"
	"$(PYTHON)" tools/ttf2pgf.py "$$@" "$(3)" --pin "$(4)" $(6) $(8) --font-name "$(7)" --font-type "$(9)" --manifest "$(PROJECT_FONT_GENERATED_DIR_PY)/$(basename $(2)).json"
	"$(PYTHON)" -c "import shutil; shutil.copyfile('$(5)', '$(PROJECT_FONT_GENERATED_DIR_PY)/$(basename $(2))-OFL.txt')"
endef

# The console's two Latin sizes, 10.125 and 7 points at 128 dpi (#2005, font/README.md). Every face of
# a size shares its strike: the size and emboldening that put the regular face's ink where the console
# font of that size has it (x-height, cap height, descender, stem), and the console fonts' header
# ascender and descender. Each face then has its own spacing, chosen so that its lines are as long as
# the console face's of the same index; the sans-serif faces also get the console's digit width.
LIBERATION := fixtures/fonts/liberation
PROJECT_LATIN_STRIKE_ARGS := --ppem 20 --embolden-x 4 --embolden-advance 4 --nominal-size 648 --resolution 8192 --header-ascender 863 --header-descender -196
PROJECT_LATIN_SMALL_STRIKE_ARGS := --ppem 13 --embolden-x 2 --embolden-advance 2 --nominal-size 448 --resolution 8192 --header-ascender 576 --header-descender -131
# Per index: source face, PGF style, then that face's spacing (--tracking, eighths of a pixel) and,
# where measured, digit advance (26.6 px).
PROJECT_LATIN_0_FACE := LiberationSans-Regular
PROJECT_LATIN_1_FACE := LiberationSerif-Regular
PROJECT_LATIN_2_FACE := LiberationSans-Italic
PROJECT_LATIN_3_FACE := LiberationSerif-Italic
PROJECT_LATIN_4_FACE := LiberationSans-Bold
PROJECT_LATIN_5_FACE := LiberationSerif-Bold
PROJECT_LATIN_6_FACE := LiberationSans-BoldItalic
PROJECT_LATIN_7_FACE := LiberationSerif-BoldItalic
PROJECT_LATIN_0_STYLE := Regular
PROJECT_LATIN_1_STYLE := Regular
PROJECT_LATIN_2_STYLE := Italic
PROJECT_LATIN_3_STYLE := Italic
PROJECT_LATIN_4_STYLE := Bold
PROJECT_LATIN_5_STYLE := Bold
PROJECT_LATIN_6_STYLE := Bold Italic
PROJECT_LATIN_7_STYLE := Bold Italic
PROJECT_LATIN_0_SPACING := --tracking 7 --digit-advance 852
PROJECT_LATIN_1_SPACING := --tracking 2
PROJECT_LATIN_2_SPACING := --tracking 7 --digit-advance 852
PROJECT_LATIN_3_SPACING := --tracking 3
PROJECT_LATIN_4_SPACING := --tracking 6 --digit-advance 840
PROJECT_LATIN_5_SPACING := --tracking 1
PROJECT_LATIN_6_SPACING := --tracking 6 --digit-advance 840
PROJECT_LATIN_7_SPACING := --tracking 2
PROJECT_LATIN_8_SPACING := --tracking 7 --digit-advance 555
PROJECT_LATIN_9_SPACING := --tracking 3
PROJECT_LATIN_10_SPACING := --tracking 7 --digit-advance 555
PROJECT_LATIN_11_SPACING := --tracking 4
PROJECT_LATIN_12_SPACING := --tracking 6 --digit-advance 559
PROJECT_LATIN_13_SPACING := --tracking 3
PROJECT_LATIN_14_SPACING := --tracking 6 --digit-advance 559
PROJECT_LATIN_15_SPACING := --tracking 3
# $(1) index, $(2) target stem, $(3) output, $(4) the face index within its size (0-7), $(5) strike.
define PROJECT_LATIN_FACE_RULE
$(call PROJECT_FONT_RULE,$(2),$(3),$(LIBERATION)/$(PROJECT_LATIN_$(4)_FACE).ttf,$(LIBERATION)/$(PROJECT_LATIN_$(4)_FACE).pin.json,$(LIBERATION)/OFL.txt,--latin-cyrillic-text,Nakagawa Open Latin,$(5) $(PROJECT_LATIN_$(1)_SPACING),$(PROJECT_LATIN_$(4)_STYLE))
endef
$(eval $(call PROJECT_LATIN_FACE_RULE,0,latin,nkltn.pgf,0,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,1,latin-1,nkltn1.pgf,1,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,2,latin-2,nkltn2.pgf,2,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,3,latin-3,nkltn3.pgf,3,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,4,latin-4,nkltn4.pgf,4,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,5,latin-5,nkltn5.pgf,5,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,6,latin-6,nkltn6.pgf,6,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,7,latin-7,nkltn7.pgf,7,$(PROJECT_LATIN_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,8,latin-small,nkltns.pgf,0,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,9,latin-9,nkltn9.pgf,1,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,10,latin-10,nkltn10.pgf,2,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,11,latin-11,nkltn11.pgf,3,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,12,latin-12,nkltn12.pgf,4,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,13,latin-13,nkltn13.pgf,5,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,14,latin-14,nkltn14.pgf,6,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))
$(eval $(call PROJECT_LATIN_FACE_RULE,15,latin-15,nkltn15.pgf,7,$(PROJECT_LATIN_SMALL_STRIKE_ARGS)))

# Japanese and Korean at the console's 10.125 points (18 pixels per em at 128 dpi). The Japanese
# font draws CP932 and both JIS kanji levels from Noto Sans JP (the UI font's pinned instance); the
# one character of that set Noto Sans JP lacks comes from M PLUS 1p. Its licence file carries both
# licences. The header ascender and descender are the console fonts'.
PROJECT_JAPANESE_FONT_ARGS := --ppem 18 --nominal-size 648 --resolution 8192 --header-ascender 1013 --header-descender -139 --baseline-ellipsis
PROJECT_KOREAN_FONT_ARGS := --ppem 20 --tracking 1 --nominal-size 648 --resolution 8192 --header-ascender 1000 --header-descender -250
NOTO_SANS_JP := assets/fonts/ui/noto-sans-jp
project-japanese-font: $(PROJECT_FONT_GENERATED_DIR)/nkjpn.pgf
$(PROJECT_FONT_GENERATED_DIR)/nkjpn.pgf: $(NOTO_SANS_JP)/NotoSansJP-Regular.ttf fixtures/fonts/notosansjp/PIN.json $(NOTO_SANS_JP)/OFL.txt fixtures/fonts/mplus1p/MPLUS1p-Regular.ttf fixtures/fonts/mplus1p/PIN.json fixtures/fonts/mplus1p/OFL.txt tools/ttf2pgf.py tools/pgf_writer.py docs/cleanroom/PGF_CONVERTER_SPEC.md
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(PROJECT_FONT_GENERATED_DIR_PY)').mkdir(parents=True, exist_ok=True)"
	"$(PYTHON)" tools/ttf2pgf.py "$@" "$(NOTO_SANS_JP)/NotoSansJP-Regular.ttf" --pin fixtures/fonts/notosansjp/PIN.json --fallback fixtures/fonts/mplus1p/MPLUS1p-Regular.ttf --fallback-pin fixtures/fonts/mplus1p/PIN.json --japanese-text $(PROJECT_JAPANESE_FONT_ARGS) --font-name "Nakagawa Open Japanese" --font-type "Regular" --manifest "$(PROJECT_FONT_GENERATED_DIR_PY)/nkjpn.json"
	"$(PYTHON)" -c "from pathlib import Path; parts = [Path(p).read_text(encoding='utf-8') for p in ('$(NOTO_SANS_JP)/OFL.txt', 'fixtures/fonts/mplus1p/OFL.txt')]; Path('$(PROJECT_FONT_GENERATED_DIR_PY)/nkjpn-OFL.txt').write_text('Noto Sans JP:\n\n' + parts[0] + '\n\nM PLUS 1p (U+2252 only):\n\n' + parts[1], encoding='utf-8')"
$(eval $(call PROJECT_FONT_RULE,korean,nkkr.pgf,fixtures/fonts/gowundodum/GowunDodum-Regular.ttf,fixtures/fonts/gowundodum/PIN.json,fixtures/fonts/gowundodum/OFL.txt,--ks-x-1001-korean,Nakagawa Open Korean,$(PROJECT_KOREAN_FONT_ARGS),Regular))

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

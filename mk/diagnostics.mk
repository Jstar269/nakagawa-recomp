# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: help, compiler-info, readiness, provenance, contribution and aggregate test targets.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# Stable diagnostic surface for CI and local setup checks. This target performs no
# compilation and makes GNU Make's selected compiler and assignment origin explicit.
compiler-info:
	@echo CC=$(CC)
	@echo CC_ORIGIN=$(origin CC)
	@echo RUNTIME_OPT=$(RUNTIME_OPT)
	@echo GE_FRONTEND_PROFILE=$(GE_FRONTEND_PROFILE)
	@echo CFLAGS=$(CFLAGS)
	@echo RECOMP_OPT=$(RECOMP_OPT)
	@echo RECOMP_FLAGS=$(RECOMP_FLAGS)
	@echo PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)
	@echo FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)
	@echo CHUNK_TARGET_BYTES=$(CHUNK_TARGET_BYTES)
	@echo PUBLIC_SAFE=$(PUBLIC_SAFE)
	@echo SDL3_DIR=$(SDL3_DIR)
	@echo SDL3_PROVIDER=$(SDL3_PROVIDER)
	@echo SDL3_VERSION=$(SDL3_VERSION)

# One command for the whole pre-pull-request checklist in docs/CI.md.
#
# The checklist has been correct and documented for a while and is still
# routinely half-run: the usual outcome is that publish_audit is run, reported
# as "gates green", and provenance_attest_verify -- the gate that actually
# blocks the merge -- is never run at all. They answer different questions.
# publish_audit compares the candidate against its own checked-in ledger, so the
# candidate supplies both sides and an unapproved blob cannot be detected.
# provenance_attest_verify compares it against the external private authority,
# which is the only thing that emits BLOB_UNAPPROVED. A tree can be
# "publication audit: OK" and "verdict: FAIL" at the same commit.
#
# Ordered cheapest-first so a policy or ledger mistake surfaces in seconds
# rather than after the suite. Every step is fatal; make stops at the first one.
#
#   NK_TRUSTED_LEDGER=/path/to/IMPLEMENTATION_PROVENANCE.json make readiness
#
# NK_TRUSTED_LEDGER is the detailed development ledger from the private history
# repository. It is deliberately required rather than defaulted: skipping the
# attestation step when the path is absent is exactly the silent pass this
# target exists to prevent, so an unset value is BLOCKED, not "not applicable".
READINESS_BASE ?= $(shell git merge-base origin/main HEAD)
READINESS_WORKDIR ?= $(CURDIR)/../.nk-readiness-verify

readiness:
ifndef NK_TRUSTED_LEDGER
	@echo "readiness: BLOCKED -- NK_TRUSTED_LEDGER is unset."
	@echo "  It must name the detailed development ledger"
	@echo "  (docs/provenance/IMPLEMENTATION_PROVENANCE.json in the private"
	@echo "  history repository). Without it the attestation gate cannot run,"
	@echo "  and an unrun gate is BLOCKED evidence, never a pass."
	@exit 1
endif
	@echo "== readiness: base $(READINESS_BASE)"
	@test -n "$(NK_GIT_REV_HEAD)" || { echo "readiness: FAIL -- no source revision: $(GIT) is unavailable or this is not a git checkout, and the provenance attestation compares a candidate revision. Readiness needs a git checkout; set GIT to a git executable if it is not on PATH."; exit 1; }
	"$(PYTHON)" tools/policy_sync.py
	"$(PYTHON)" tools/lint_docs.py
	"$(PYTHON)" tools/provenance_record_gap.py --check
	"$(PYTHON)" tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
	"$(PYTHON)" tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
	"$(PYTHON)" tools/provenance_attest_verify.py --repo . --candidate "$(NK_GIT_REV_HEAD)" --base $(READINESS_BASE) --require-immutable-revisions --trusted-ledger "$(NK_TRUSTED_LEDGER)" --workdir "$(READINESS_WORKDIR)"
	git diff --check $(READINESS_BASE)..HEAD
	@echo "== readiness: control-file completeness"
	"$(PYTHON)" tools/policy_sync.py --regen-export
	git diff --quiet -- PUBLIC_EXPORT.json assets/public_provenance_ledger.json assets/public_source_profile.json || { echo "readiness: FAIL -- regenerating the control files changed them, so the commit does not carry the evidence for its own contents. Stage and commit assets/public_provenance_ledger.json, assets/public_source_profile.json and PUBLIC_EXPORT.json."; exit 1; }
	@echo "== readiness: OK (the suite is separate and still yours to run)"

# One-command local refresh using the hosted attestation's control generator.
# Stage intended candidate changes first; this target stages only the generated
# profile (when requested) and the two generated controls. The external
# authority remains mandatory. Set PROVENANCE_REFRESH_APPLY_POLICY=1 only when
# the maintainer has already decided that newly seen routine paths belong on
# the public surface.
PROVENANCE_REFRESH_APPLY_POLICY ?= 0
PROVENANCE_REFRESH_POLICY_ARG = $(if $(filter 1 true yes,$(PROVENANCE_REFRESH_APPLY_POLICY)),--apply-policy,)

provenance-refresh:
ifndef NK_TRUSTED_LEDGER
	@echo "provenance-refresh: TRUSTED_INPUT_MISSING -- NK_TRUSTED_LEDGER is unset."
	@echo "  Set it to the external IMPLEMENTATION_PROVENANCE.json and retry."
	@echo "  Stage intended candidate changes before running this target."
	@exit 1
else
	"$(PYTHON)" tools/provenance_refresh.py --trusted-ledger "$(NK_TRUSTED_LEDGER)" $(PROVENANCE_REFRESH_POLICY_ARG)
endif

provenance-record-gap-check:
	"$(PYTHON)" tools/provenance_record_gap.py --check

# The contributor fast path: one command, only the gates that apply to the
# files this branch changed against origin/main, finished in minutes.
#
# It is a subset of `check`, never a substitute: `check` and `readiness` remain
# the authoritative gates, and CI still runs the hosted matrix. What it removes
# is the reason a contributor gives up -- being told to run the whole native and
# tooling suite to find out whether a one-line documentation change is well
# formed.
#
# Path routing is tools/ci_paths.py, the same classifier the hosted workflow
# uses, so the local selection cannot drift from the hosted one. The gates it
# runs are the ones a contributor can actually clear: Ruff, the Python test
# modules matching the changed tools/, a strict C compile of the changed C, and
# markdownlint plus the documentation-freshness lint for changed Markdown.
#
# The publication audit's provenance self-consistency leg compares the working
# tree with the checked-in ledger, which only a maintainer holding the private
# trusted ledger can regenerate. Those findings are printed as MAINTAINER-SIDE
# and do not fail this target; every other publication finding does. Nothing is
# suppressed -- both lists are always shown.
#
#   mingw32-make contrib-check              # Windows
#   make contrib-check                      # Linux
#   CONTRIB_BASE=origin/main contrib-check  # explicit base
#
# Reports PASS, FAIL, SKIP (tool not installed) or NOT_RUN (surface untouched)
# per gate. A skipped gate is never reported as a pass.
CONTRIB_BASE ?= origin/main

# The sub-gates run in the same interpreter as this script by default. Set
# CONTRIB_PYTHON in the environment when the repository's default python is not
# the one holding ruff and the project dependencies -- which is the normal case
# on Windows, where MSYS2's python precedes the CPython the project uses. Keep
# this as an exported environment variable so the nested gates use the same
# interpreter as tools/contrib_check.py; Make recipes quote $(PYTHON) so paths
# with spaces survive the POSIX shell.
#
# GNU Make target-specific export exports CONTRIB_PYTHON into the recipe's
# child process environment across platforms, avoiding POSIX shell syntax
# (e.g. VAR=val cmd) that fails under Windows cmd.exe.
CONTRIB_PYTHON ?= $(PYTHON)
contrib-check: export CONTRIB_PYTHON := $(CONTRIB_PYTHON)
contrib-check:
	"$(PYTHON)" tools/contrib_check.py --base "$(CONTRIB_BASE)"

# Reproduce the two public provenance controls for the paths your change touches,
# so an ordinary pull request does not wait on a maintainer for the ordinary case.
#
# It is a convenience, not an authority. It copies the claim already recorded for
# each changed path that has a public entry and rehashes only that path, then
# stages PUBLIC_EXPORT.json and assets/public_provenance_ledger.json. It cannot
# see the private trusted ledger, so it cannot see frozen historical debt or a
# changed executable path with no public entry; it refuses rather than guess when
# a changed path is new or removed, when its entry is unresolved, or when the
# source policy profile itself changed. The maintainer's trusted attestation
# re-derives both files from the private authority at landing, and that
# re-derivation -- not this target -- is what admits the change.
#
#   mingw32-make contrib-refresh              # Windows
#   make contrib-refresh                      # Linux
#   CONTRIB_BASE=origin/main contrib-refresh  # explicit base
#   make contrib-refresh CONTRIB_REFRESH_CHECK=--check   # report, write nothing
#
# Nothing is written when --check is passed, and nothing is written when the tool
# refuses. A refusal prints its code and exits non-zero.
CONTRIB_REFRESH_CHECK ?=
contrib-refresh:
	"$(PYTHON)" tools/contrib_refresh.py --base "$(CONTRIB_BASE)" $(CONTRIB_REFRESH_CHECK)

public-safe-verify:
	$(MAKE) PUBLIC_SAFE=1 portable-core-objects

# Which tracked files does nothing in the tree name?  The report is the point:
# a candidate for removal is a judgement call about a boundary, a provenance
# record or a legal file, and it belongs to a human reviewing a diff rather than
# to a build that happens to be running.  So this target reports and exits 0.
# CI runs `python tools/tree_lint.py --check` in the always-on hygiene job.
# This target stays reporting-only so local inspection can show findings without
# failing solely because it found candidates.
tree-lint:
	"$(PYTHON)" tools/tree_lint.py

# -----------------------------------------------------------------------------
# Public Verification Entry Points (Issue #188 Finding 3 O-05)
# -----------------------------------------------------------------------------
test:
	"$(PYTHON)" -m unittest discover -s tools -p "test_*.py" -v

check:
	@echo "== [1/7] Documentation freshness lint =="
	"$(PYTHON)" tools/lint_docs.py
	@echo "== [2/7] Canonical publication policy coverage =="
	"$(PYTHON)" -m unittest tools/test_publication_policy_gate.py
	@echo "== [3/7] Provenance record and disposition gap check =="
	"$(PYTHON)" tools/provenance_record_gap.py --check
	@echo "== [4/7] Publication safety audits (index & worktree) =="
	"$(PYTHON)" tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
	"$(PYTHON)" tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
	@echo "== [5/7] Native host core tests =="
	$(MAKE) native-core-tests
	@echo "== [6/7] Format portability check (32-bit newlib simulation) =="
	"$(PYTHON)" tools/format_portability_check.py
	@echo "== [7/7] Fast critical test subset =="
	"$(PYTHON)" -m unittest \
		tools/test_title_manifest.py \
		tools/test_title_catalog.py \
		tools/test_build_system_parity.py \
		tools/test_sync_drift_check.py \
		tools/test_ci_paths.py \
		tools/test_ci_required.py \
		tools/test_provenance_record_gap.py
	@echo "== All public verification checks PASSED =="


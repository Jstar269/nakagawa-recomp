#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deprecated forwarding wrapper. Use tools/nk_doctor_checks.py instead.

hst_doctor_checks.py is a deprecated forwarding wrapper for nk_doctor_checks.py
(issue #196 Phase 3). All names are re-exported from the canonical module so
that callers using `import hst_doctor_checks` and `mock.patch.object` continue
to work during the deprecation window.

This file will be removed in the Phase 5 rename sweep; update any scripts or
documentation that reference it (#196).
"""

from __future__ import annotations

import importlib
import sys
import warnings

warnings.warn(
    "hst_doctor_checks is deprecated (issue #196); use nk_doctor_checks instead.",
    DeprecationWarning,
    stacklevel=2,
)

# Keep the legacy import name as an actual module alias.  A star-imported
# forwarding surface copies function objects, so mock.patch.object() against
# hst_doctor_checks does not change the globals those functions resolve.  An
# alias preserves both the old import path and patching/monkey-patching
# semantics while the canonical module remains the sole implementation.
_canonical = importlib.import_module("nk_doctor_checks")
sys.modules[__name__] = _canonical

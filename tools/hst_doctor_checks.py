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

import warnings

warnings.warn(
    "hst_doctor_checks is deprecated (issue #196); use nk_doctor_checks instead.",
    DeprecationWarning,
    stacklevel=2,
)

# Re-export all public names.
from nk_doctor_checks import *  # noqa: F401, F403, E402

# Re-export public check functions explicitly for IDE support.
from nk_doctor_checks import (  # noqa: F401, E402
    check_agent_identity,
    check_build_products,
    check_build_profile,
    check_platform,
    check_private_inputs,
    check_repository_contract,
    check_runtime_dependencies,
    check_save_root,
    check_toolchain,
    check_vfpu_assets,
)

# Re-export private helpers needed by tests that use mock.patch.object(hst_doctor_checks, ...).
# These are stable implementation details that tests must patch on the module they import.
from nk_doctor_checks import (  # noqa: F401, E402
    _find_executable,
    _probe_powershell,
    _run_version,
)

# Optional helpers that may not exist in all versions; ignore ImportError.
try:
    from nk_doctor_checks import _probe_windows_info  # noqa: F401
except ImportError:
    pass

try:
    from nk_doctor_checks import check_shader_provenance  # noqa: F401
except ImportError:
    pass

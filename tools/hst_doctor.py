#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deprecated forwarding wrapper. Use tools/nk_doctor.py instead.

hst_doctor.py is a deprecated forwarding wrapper for nk_doctor.py (issue #196
Phase 3). All imports, the CLI entry point, and the module-level names are
re-exported from nk_doctor and nk_doctor_checks so that any script importing or
invoking hst_doctor continues to work during the deprecation window.

This file will be removed in the Phase 5 rename sweep; update any scripts or
documentation that reference it (#196).
"""

from __future__ import annotations

import sys
import warnings

warnings.warn(
    "hst_doctor is deprecated (issue #196); use nk_doctor instead.",
    DeprecationWarning,
    stacklevel=2,
)

# Re-export everything from the canonical entry-point module so
# `from hst_doctor import X` and `hst_doctor.X(...)` continue to work.
from nk_doctor import (  # noqa: F401, E402
    build_parser,
    main,
    render_json,
    render_text,
)

# Re-export core types so `hst_doctor.Report(...)` continues to work.
from nk_doctor_core import Report, _parse_elf, _validate_iso, _validate_pe_x64  # noqa: F401, E402

# Re-export all check functions so `hst_doctor.check_*()` continues to work.
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

if __name__ == "__main__":
    raise SystemExit(main())

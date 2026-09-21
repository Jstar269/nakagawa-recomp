#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deprecated forwarding wrapper. Use tools/nk_doctor_core.py instead.

hst_doctor_core.py is a deprecated forwarding wrapper for nk_doctor_core.py (issue
#196 Phase 3). All names are re-exported from the canonical module.

This file will be removed in the Phase 5 rename sweep; update any scripts or
documentation that reference it (#196).
"""

from __future__ import annotations

import warnings

warnings.warn(
    "hst_doctor_core is deprecated (issue #196); use nk_doctor_core instead.",
    DeprecationWarning,
    stacklevel=2,
)

from nk_doctor_core import *  # noqa: F401, F403, E402
from nk_doctor_core import (  # noqa: F401, E402
    EXPECTED_ELF_MACHINE,
    EXPECTED_VFPU_FILES,
    PRIVATE_EXTENSIONS,
    PRIVATE_PREFIXES,
    PT_LOAD,
    Report,
    _bounded_nonempty_directory,
    _find_executable,
    _parse_elf,
    _parse_psp_header,
    _run_version,
    _scan_disc_id,
    _validate_iso,
    _validate_pe_x64,
)

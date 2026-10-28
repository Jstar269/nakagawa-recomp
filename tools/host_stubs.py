# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

# Semantic name overrides for known HST functions. Imported by codegen.py so
# the same table is available to analysis tools (nidseq.py, analyze.py)
# without depending on the translator.
#
# Key  : guest address (int)
# Value: (human-readable name (str), return_value (int))
#
# The return value for each entry: 1 for functions that return a meaningful
# value to the caller, 0 for VFS/world/txcode entries that mean "no-op"
# (just return success and do nothing). Keep real container/memory-management
# routines out of this list: callers depend on their writes, not only r2.

# Retired under #363 (2026-10-27): the last five entries (0x0001c010, 0x0001c0fc,
# 0x0001c104, 0x0001c560, 0x0001c604) were the title's archive open/close/seek/read path.
# With the UMD block device readable in sectors, each now translates as written.
HST_SIMPLE_STUBS: dict = {}

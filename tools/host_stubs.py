# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

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

HST_SIMPLE_STUBS = {
    0x00015f98: ("Config_LoadGameSettings", 1),
    0x0001c010: ("VFS_RegisterHeap",      0),
    0x0001c0fc: ("VFS_RegisterBuffer",    0),
    0x0001c104: ("Config_LoadProfile",    1),
    0x0001c810: ("TexCache_Initialize",   0),
    0x0001c818: ("VFS_RegisterCallback",  0),
    0x0001c560: ("main_GraphicsInit",     1),
    0x0001c604: ("World_LoadInitialState", 0),
}

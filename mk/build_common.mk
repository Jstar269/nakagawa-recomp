# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

# Keep compiler-discovered transitive header dependencies beside each object.
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@

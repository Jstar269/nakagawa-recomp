# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

# Keep compiler-discovered transitive header dependencies beside each object.
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
#version 450

/* Reserved for the Vulkan parity backend. C3a renders waves through the CPU reference. */
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(0.0);
}

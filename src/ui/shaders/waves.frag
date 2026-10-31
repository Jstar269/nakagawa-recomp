// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
#version 450

/* Reserved for drawing the waves on the GPU. The SDL_Renderer and Vulkan backends both
   draw them through the CPU reference (nk_ui_waves_render_command); nothing uses this yet. */
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(0.0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
#version 450

layout(set = 0, binding = 0) uniform sampler2D ui_texture;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(ui_texture, in_uv) * in_color;
}

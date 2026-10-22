// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
#version 450

layout(location = 0) in vec2 in_position_px;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform UiTransform {
    vec2 output_size_px;
} transform;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    vec2 normalized = in_position_px / transform.output_size_px;
    gl_Position = vec4(normalized.x * 2.0 - 1.0,
                       1.0 - normalized.y * 2.0, 0.0, 1.0);
    out_uv = in_uv;
    out_color = in_color;
}

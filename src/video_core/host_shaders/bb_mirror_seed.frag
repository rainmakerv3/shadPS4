// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_EXT_samplerless_texture_functions : require

// Copies the game's depth, or one stencil bit, into the velocity mirror's larger depth image with
// nearest scaling, like vkCmdBlitImage. For GPUs that cannot blit into depth formats.
#ifdef STENCIL
layout(binding = 0) uniform utexture2D source;
#else
layout(binding = 0) uniform texture2D source;
#endif

layout(push_constant) uniform Push {
    vec2 scale; // source texels per target pixel
    uint bit;   // stencil bit this pass writes
} pc;

void main() {
    const ivec2 texel = ivec2(gl_FragCoord.xy * pc.scale);
#ifdef STENCIL
    if (((texelFetch(source, texel, 0).r >> pc.bit) & 1u) == 0u) {
        discard;
    }
#else
    gl_FragDepth = texelFetch(source, texel, 0).r;
#endif
}

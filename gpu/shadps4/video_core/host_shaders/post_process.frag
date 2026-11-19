// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    bool easu; ///< FSR 1 EASU when the frame is smaller than the output (sharpening=1)
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// One texel as stored: sRGB-encoded UNORM input stays encoded (perceptual, as EASU expects).
vec4 fetch_texel(ivec2 p) { return texelFetch(texSampler, p, 0); }

vec4 fetch(vec2 at) {
    vec4 c = texture(texSampler, at);
    if (pp.srgb_input && !pp.hdr) c.rgb = degamma(c.rgb);
    return c;
}


// resogun: AMD FidelityFX FSR 1 EASU (edge-adaptive spatial upsampling), after FsrEasuF in
// ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT), 32-bit float path. Used when
// the frame is smaller than the output (PS4 Pro mode's 3200x1800 on a 3840x2160 screen): 12 texels
// around the sample point give a local edge direction and length, and a Lanczos-like lobe stretched
// along that edge weights them, clamped to the 2x2 nearest texels' range so it cannot ring.
// RCAS (rcas.frag) then sharpens the result, as FSR 1 runs the two.
vec4 easu_tap(ivec2 p, ivec2 size) { return fetch_texel(clamp(p, ivec2(0), size - 1)); }
float easu_luma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void easu_set(inout vec2 dir, inout float len, float w,
              float lA, float lB, float lC, float lD, float lE) {
    const float dc = lD - lC, cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0 / 32768.0);
    const float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    len += lenX * lenX * w;
    const float ec = lE - lC, ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0 / 32768.0);
    const float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    len += lenY * lenY * w;
}

void easu_accumulate(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob,
                     float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    const float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    const float w = wB * wA;
    aC += c * w;
    aW += w;
}

// out_px: the output pixel's centre; out_size: the output's size in pixels.
vec4 easu(vec2 out_px, vec2 out_size) {
    const ivec2 size = textureSize(texSampler, 0);
    vec2 q = out_px * (vec2(size) / out_size) - 0.5;
    const vec2 fp = floor(q);
    q -= fp;
    const ivec2 f0 = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    const vec4 b = easu_tap(f0 + ivec2(0, -1), size), c = easu_tap(f0 + ivec2(1, -1), size);
    const vec4 e = easu_tap(f0 + ivec2(-1, 0), size), f = easu_tap(f0, size);
    const vec4 g = easu_tap(f0 + ivec2(1, 0), size), h = easu_tap(f0 + ivec2(2, 0), size);
    const vec4 i = easu_tap(f0 + ivec2(-1, 1), size), j = easu_tap(f0 + ivec2(0, 1), size);
    const vec4 k = easu_tap(f0 + ivec2(1, 1), size), l = easu_tap(f0 + ivec2(2, 1), size);
    const vec4 n = easu_tap(f0 + ivec2(0, 2), size), o = easu_tap(f0 + ivec2(1, 2), size);
    const float bL = easu_luma(b.rgb), cL = easu_luma(c.rgb), eL = easu_luma(e.rgb), fL = easu_luma(f.rgb);
    const float gL = easu_luma(g.rgb), hL = easu_luma(h.rgb), iL = easu_luma(i.rgb), jL = easu_luma(j.rgb);
    const float kL = easu_luma(k.rgb), lL = easu_luma(l.rgb), nL = easu_luma(n.rgb), oL = easu_luma(o.rgb);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - q.x) * (1.0 - q.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, q.x * (1.0 - q.y), cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - q.x) * q.y, fL, iL, jL, kL, nL);
    easu_set(dir, len, q.x * q.y, gL, jL, kL, lL, oL);
    const vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    const float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    const vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    const float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    const float clp = 1.0 / lob;
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easu_accumulate(aC, aW, vec2(0.0, -1.0) - q, dir, len2, lob, clp, b.rgb);
    easu_accumulate(aC, aW, vec2(1.0, -1.0) - q, dir, len2, lob, clp, c.rgb);
    easu_accumulate(aC, aW, vec2(-1.0, 1.0) - q, dir, len2, lob, clp, i.rgb);
    easu_accumulate(aC, aW, vec2(0.0, 1.0) - q, dir, len2, lob, clp, j.rgb);
    easu_accumulate(aC, aW, vec2(0.0, 0.0) - q, dir, len2, lob, clp, f.rgb);
    easu_accumulate(aC, aW, vec2(-1.0, 0.0) - q, dir, len2, lob, clp, e.rgb);
    easu_accumulate(aC, aW, vec2(1.0, 1.0) - q, dir, len2, lob, clp, k.rgb);
    easu_accumulate(aC, aW, vec2(2.0, 1.0) - q, dir, len2, lob, clp, l.rgb);
    easu_accumulate(aC, aW, vec2(2.0, 0.0) - q, dir, len2, lob, clp, h.rgb);
    easu_accumulate(aC, aW, vec2(1.0, 0.0) - q, dir, len2, lob, clp, g.rgb);
    easu_accumulate(aC, aW, vec2(1.0, 2.0) - q, dir, len2, lob, clp, o.rgb);
    easu_accumulate(aC, aW, vec2(0.0, 2.0) - q, dir, len2, lob, clp, n.rgb);
    const vec3 mn = min(min(f.rgb, g.rgb), min(j.rgb, k.rgb));
    const vec3 mx = max(max(f.rgb, g.rgb), max(j.rgb, k.rgb));
    return vec4(min(mx, max(mn, aC / aW)), f.a);
}

// resogun: a frame larger than the output (render_resolution=4k on a smaller screen) is
// filtered down over each output pixel's whole footprint: a grid of up to 4x4 bilinear taps spread
// across it, averaged in linear light. One bilinear tap reads only 2x2 texels, so at 4K -> 1440p
// (1.5 texels per pixel) it skipped texels and edges shimmered. At or below 1:1 it is one tap.
void main() {
    vec2 footprint = fwidth(uv) * vec2(textureSize(texSampler, 0));
    if (pp.easu && footprint.x < 0.999 && footprint.y < 0.999) {
        vec4 up = easu(gl_FragCoord.xy, vec2(1.0) / fwidth(uv));
        if (pp.srgb_input && !pp.hdr) up.rgb = degamma(up.rgb);
        color = pp.hdr ? up : vec4(gamma(up.rgb), up.a);
        return;
    }
    ivec2 taps = ivec2(clamp(ceil(footprint - 0.01), vec2(1.0), vec2(4.0)));
    vec2 step_uv = fwidth(uv) / vec2(taps);
    vec4 color_linear = vec4(0.0);
    for (int y = 0; y < taps.y; ++y) {
        for (int x = 0; x < taps.x; ++x) {
            color_linear += fetch(uv + (vec2(x, y) + 0.5 - vec2(taps) * 0.5) * step_uv);
        }
    }
    color_linear /= float(taps.x * taps.y);
    if (pp.hdr) {
        color = color_linear;
    } else {
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}

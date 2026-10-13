#version 440
// One layer (or a cross dissolve of two) drawn onto the canvas, as render/Compositor's transformLayer, transitionMix
// and render/Processing's blendOnto do it: the layer's picture placed by its transform (bilinear, edges clamped, its
// crop and frame edges anti-aliased by coverage, its opacity), then blended in its mode.
layout(location = 0) out vec4 fragColor;
// p[0]: x canvas has a picture, y the layer replaces it (the first normal layer), z blend mode, w the dissolve's
// position (negative: one layer). Each layer's placement from p[1] (the first) and p[6] (the second):
//   [0] kind (0 nothing, 1 already the canvas's size, 2 placed by its transform), scale, opacity, axis aligned
//   [1] centre x, centre y (sequence pixels), cos, sin    [2] scale x, scale y, anchor u, anchor v (media pixels)
//   [3] crop u0, u1, v0, v1    [4] image pixels per media pixel x, y; output pixels per media pixel x, y
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D canvasTex;
layout(binding = 2) uniform sampler2D layerA;
layout(binding = 3) uniform sampler2D layerB;

vec4 layerAt(sampler2D t, int b, ivec2 xy) {
    const int kind = int(p[b].x);
    if (kind == 0) return vec4(0.0);
    const ivec2 size = textureSize(t, 0);
    if (kind == 1) return xy.x < size.x && xy.y < size.y ? texelFetch(t, xy, 0) : vec4(0.0);
    const float scale = p[b].y;
    const float pxs = (float(xy.x) + 0.5) / scale - p[b + 1].x, pys = (float(xy.y) + 0.5) / scale - p[b + 1].y;
    const bool axis = p[b].w > 0.5;
    const float rx = axis ? pxs : pxs * p[b + 1].z + pys * p[b + 1].w;
    const float ry = axis ? pys : -pxs * p[b + 1].w + pys * p[b + 1].z;
    const float u = rx / p[b + 2].x + p[b + 2].z, v = ry / p[b + 2].y + p[b + 2].w;
    const float u0 = p[b + 3].x, u1 = p[b + 3].y, v0 = p[b + 3].z, v1 = p[b + 3].w;
    if (u < u0 - 1.0 || u > u1 + 1.0 || v < v0 - 1.0 || v > v1 + 1.0) return vec4(0.0);
    const float mx = p[b + 4].z, my = p[b + 4].w;
    const float cov = clamp((u - u0) * mx + 0.5, 0.0, 1.0) * clamp((u1 - u) * mx + 0.5, 0.0, 1.0) *
                      clamp((v - v0) * my + 0.5, 0.0, 1.0) * clamp((v1 - v) * my + 0.5, 0.0, 1.0);
    if (cov <= 0.0) return vec4(0.0);
    const float ix = u * p[b + 4].x - 0.5, iy = v * p[b + 4].y - 0.5;
    const int x0 = int(floor(ix)), y0 = int(floor(iy));
    const float fx = ix - float(x0), fy = iy - float(y0);
    const int xa = clamp(x0, 0, size.x - 1), xb = clamp(x0 + 1, 0, size.x - 1);
    const int ya = clamp(y0, 0, size.y - 1), yb = clamp(y0 + 1, 0, size.y - 1);
    const vec4 p00 = texelFetch(t, ivec2(xa, ya), 0), p10 = texelFetch(t, ivec2(xb, ya), 0);
    const vec4 p01 = texelFetch(t, ivec2(xa, yb), 0), p11 = texelFetch(t, ivec2(xb, yb), 0);
    const vec4 top = p00 + (p10 - p00) * fx, bot = p01 + (p11 - p01) * fx;
    return (top + (bot - top) * fy) * (p[b].z * cov);
}

float blendChannel(int mode, float cb, float cs) {
    if (mode == 1) return cb + cs;
    if (mode == 2) return cb * cs;
    if (mode == 3) return cb + cs - cb * cs;
    if (mode == 4) return cb <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
    if (mode == 5) return min(cb, cs);
    if (mode == 6) return max(cb, cs);
    if (mode == 7) return abs(cb - cs);
    if (mode == 8) {
        if (cs <= 0.5) return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        const float dcb = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(max(cb, 0.0));
        return cb + (2.0 * cs - 1.0) * (dcb - cb);
    }
    if (mode == 9) return cs <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
    if (mode == 10) return cs >= 1.0 ? 1.0 : min(1.0, cb / (1.0 - cs));
    if (mode == 11) return cs <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - cb) / cs);
    if (mode == 12) return max(0.0, cb - cs);
    return cs;
}

void main() {
    const ivec2 xy = ivec2(gl_FragCoord.xy);
    vec4 s = layerAt(layerA, 1, xy);
    if (p[0].w >= 0.0) s = s * (1.0 - p[0].w) + layerAt(layerB, 6, xy) * p[0].w;
    if (p[0].y > 0.5) {
        fragColor = s;
        return;
    }
    const vec4 d = p[0].x > 0.5 ? texelFetch(canvasTex, xy, 0) : vec4(0.0);
    const float as = s.a;
    if (as <= 0.0) {
        fragColor = d;
        return;
    }
    const int mode = int(p[0].z);
    if (mode == 0) {
        const float k = 1.0 - as;
        fragColor = vec4(s.rgb + d.rgb * k, as + d.a * k);
        return;
    }
    const float ab = d.a, invS = 1.0 / s.a, invB = ab > 0.0 ? 1.0 / ab : 0.0;
    vec3 o;
    for (int c = 0; c < 3; ++c) {
        const float cs = s[c] * invS, cb = d[c] * invB;
        o[c] = cs * as * (1.0 - ab) + d[c] * (1.0 - as) + as * ab * blendChannel(mode, cb, cs);
    }
    fragColor = vec4(o, as + ab * (1.0 - as));
}

#version 440
// A pass of an effect that reads beyond its own pixel, on a layer at its source size, as render/Processing and
// render/VideoFx do it on the CPU (premultiplied colour throughout):
//   1  a box blur along a row (p[1].y 0) or a column (1) of radius p[1].x, edges clamped, in the channels p[2] says
//      (the others as they are): three of them a direction are gaussianBlur's Gaussian (Gaussian Blur, Channel Blur,
//      and the blur in Sharpen and Glow)
//   2  Directional Blur: p[1].y taps along (p[1].z, p[1].w) over p[1].x pixels, bilinear, transparent outside
//   3  Sharpen's last step: the picture (orig) pushed away from its blur (srcTex) by p[1].x, within 0..4 alpha
//   4  Glow's first step: what is brighter than p[1].x, scaled by how much
//   5  Glow's last step: the blur of that (srcTex) added to the picture (orig) by p[1].x
//   6  Vignette (p[1] amount, size, softness), 7 Posterize (p[1].x levels less one), 8 Letterbox (p[1] aspect, opacity),
//   9  Mirror (p[1].x mode), 10 Mosaic (p[1].x cell size; along rows, then columns, p[1].y), 11 Chromatic Aberration (p[1].x amount in pixels),
//   12 Noise (p[1] amount, colour, kept within black and white; p[2] the frame's low and high 16 bits)
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D srcTex;  // RGBA32F, premultiplied: what the pass before made
layout(binding = 4) uniform sampler2D orig;    // RGBA32F: the picture as the effect began

float clamp01(float v) { return v > 0.0 ? min(v, 1.0) : 0.0; }
float smoothstepCpu(float e0, float e1, float x) {
    if (e1 == e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

// The texel at (x, y), transparent outside.
vec4 texel(ivec2 size, int x, int y) {
    return x < 0 || y < 0 || x >= size.x || y >= size.y ? vec4(0.0) : texelFetch(srcTex, ivec2(x, y), 0);
}
// Bilinear at (x, y) (pixel centres at +0.5), transparent outside (render/VideoFx's sample).
vec4 bilinear(ivec2 size, float x, float y) {
    x -= 0.5;
    y -= 0.5;
    const int x0 = int(floor(x)), y0 = int(floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    const vec4 p00 = texel(size, x0, y0), p10 = texel(size, x0 + 1, y0), p01 = texel(size, x0, y0 + 1), p11 = texel(size, x0 + 1, y0 + 1);
    const vec4 top = p00 + (p10 - p00) * fx, bottom = p01 + (p11 - p01) * fx;
    return top + (bottom - top) * fy;
}

uint hashMix(uint x, uint y, uint t, uint c) {
    uint h = x * 0x8da6b343u ^ y * 0xd8163841u ^ t * 0xcb1ab31fu ^ c * 0x165667b1u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

void main() {
    const ivec2 xy = ivec2(gl_FragCoord.xy);
    const ivec2 size = textureSize(srcTex, 0);
    const int mode = int(p[0].x);
    const vec4 px = texelFetch(srcTex, xy, 0);
    if (mode == 1) {
        const int r = int(p[1].x);
        const bool column = p[1].y > 0.5;
        const int n = column ? size.y : size.x;
        const int at = column ? xy.y : xy.x;
        vec4 acc = vec4(0.0);
        for (int i = -r; i <= r; ++i) {
            const int k = clamp(at + i, 0, n - 1);
            acc += texelFetch(srcTex, column ? ivec2(xy.x, k) : ivec2(k, xy.y), 0);
        }
        const vec4 blurred = acc * (1.0 / float(2 * r + 1));
        fragColor = mix(px, blurred, greaterThan(p[2], vec4(0.5)));
        return;
    }
    if (mode == 2) {
        const int taps = int(p[1].y);
        vec4 acc = vec4(0.0);
        for (int k = 0; k < taps; ++k) {
            const float s = (float(k) / float(taps - 1) - 0.5) * p[1].x;
            acc += bilinear(size, float(xy.x) + 0.5 + p[1].z * s, float(xy.y) + 0.5 + p[1].w * s);
        }
        fragColor = acc / float(taps);
        return;
    }
    if (mode == 3) {
        const vec4 o = texelFetch(orig, xy, 0);
        const float top = max(o.a, 1e-6) * 4.0;
        vec3 v = o.rgb + p[1].x * (o.rgb - px.rgb);
        fragColor = vec4(clamp(v, vec3(0.0), vec3(top)), o.a);
        return;
    }
    if (mode == 4) {
        const float a = px.a;
        const float l = a > 0.0 ? (0.2126 * px.r + 0.7152 * px.g + 0.0722 * px.b) / a : 0.0;
        const float k = l > p[1].x ? (l - p[1].x) / max(1e-4, 1.0 - p[1].x) : 0.0;
        fragColor = px * k;
        return;
    }
    if (mode == 5) {
        const vec4 o = texelFetch(orig, xy, 0);
        fragColor = vec4(o.rgb + px.rgb * p[1].x, min(1.0, o.a + px.a * p[1].x));
        return;
    }
    if (mode == 6) {
        const float cx = float(size.x) * 0.5, cy = float(size.y) * 0.5;
        const float norm = sqrt(cx * cx + cy * cy);
        const float dx = float(xy.x) + 0.5 - cx, dy = float(xy.y) + 0.5 - cy;
        const float d = sqrt(dx * dx + dy * dy) / norm;
        const float k = 1.0 - p[1].x * smoothstepCpu(p[1].y - p[1].z * 0.5, p[1].y + p[1].z * 0.5, d);
        fragColor = vec4(px.rgb * k, px.a);
        return;
    }
    if (mode == 7) {
        const float a = px.a, n = p[1].x;
        if (a <= 0.0) {
            fragColor = px;
            return;
        }
        vec3 o;
        for (int c = 0; c < 3; ++c) o[c] = floor(clamp(px[c] / a, 0.0, 1.0) * n + 0.5) / n * a;
        fragColor = vec4(o, a);
        return;
    }
    if (mode == 8) {
        const float want = p[1].x, have = float(size.x) / float(max(1, size.y)), op = p[1].y;
        bool bar = false;
        if (want > have) {
            const int b = int(floor((float(size.y) - float(size.x) / want) / 2.0 + 0.5));
            bar = xy.y < b || xy.y >= size.y - b;
        } else if (want < have) {
            const int b = int(floor((float(size.x) - float(size.y) * want) / 2.0 + 0.5));
            bar = xy.x < b || xy.x >= size.x - b;
        }
        fragColor = bar ? vec4(px.rgb * (1.0 - op), px.a * (1.0 - op) + op) : px;
        return;
    }
    if (mode == 9) {
        const int m = int(p[1].x), w = size.x, h = size.y;
        ivec2 s = xy;
        bool write;
        if (m == 0) write = xy.x >= w / 2, s.x = w - 1 - xy.x;
        else if (m == 1) write = xy.x < w / 2, s.x = w - 1 - xy.x;
        else if (m == 2) write = xy.y >= h / 2, s.y = h - 1 - xy.y;
        else write = xy.y < h / 2, s.y = h - 1 - xy.y;
        fragColor = write ? texelFetch(srcTex, s, 0) : px;
        return;
    }
    if (mode == 10) {
        // (separable: the cell's span along the row, then those along the column)
        const int s = int(p[1].x);
        const bool column = p[1].y > 0.5;
        const int at = column ? xy.y : xy.x, n = column ? size.y : size.x;
        const int b = at / s * s, e = min(n, b + s);
        vec4 acc = vec4(0.0);
        for (int k = b; k < e; ++k) acc += texelFetch(srcTex, column ? ivec2(xy.x, k) : ivec2(k, xy.y), 0);
        fragColor = acc / float(e - b);
        return;
    }
    if (mode == 11) {
        const float cx = float(size.x) / 2.0, cy = float(size.y) / 2.0, k = p[1].x / sqrt(cx * cx + cy * cy);
        const float ox = float(xy.x) + 0.5 - cx, oy = float(xy.y) + 0.5 - cy;
        const vec4 r = bilinear(size, cx + ox / (1.0 + k), cy + oy / (1.0 + k));
        const vec4 b = bilinear(size, cx + ox / (1.0 - k), cy + oy / (1.0 - k));
        fragColor = vec4(r.r, px.g, b.b, max(px.a, max(r.a, b.a)));
        return;
    }
    if (mode == 12) {
        const float a = px.a;
        if (a <= 0.0) {
            fragColor = px;
            return;
        }
        const uint frame = uint(p[2].x) + uint(p[2].y) * 65536u;
        const bool colour = p[1].y > 0.5, keep = p[1].z > 0.5;
        vec3 o;
        for (int c = 0; c < 3; ++c) {
            const float n = (float(hashMix(uint(xy.x), uint(xy.y), frame, colour ? uint(c) : 0u)) / 4294967296.0 - 0.5) * p[1].x;
            float v = px[c] / a + n;
            if (keep) v = clamp(v, 0.0, 1.0);
            o[c] = v * a;
        }
        fragColor = vec4(o, a);
        return;
    }
    fragColor = px;
}

#version 440
// The filter passes of the effects that came to the GPU last, in a shader of their own beside gpufilter.frag (whose
// bindings and parameters they share, as render/Processing makes their passes; the helpers below are gpufilter.frag's):
// 73 Magnify (render/VideoFx's magnify: p[1] the centre, radius and magnification, p[2] feather, border, opacity and
// whether square, p[3] the border's colour); Drop Shadow (render/Processing's dropShadow): 74 the shadow, the layer's
// coverage p[1].xy pixels away, times p[1].z, in p[2]'s colour (then blurred by 1's boxes), 75 the layer (orig) over
// it; 76 Broadcast Safe (render/QualityCheck's broadcastSafe: p[1] strict, knee, stripes where out of range).
// 77 Lens Correction (render/VideoFx's lensCorrection by its LensPlan: p[1] kind, the transfer function, the lens's
// centre; p[2] calibration units per pixel across and down, the plan's step and whether red and blue have their own
// places; the table's row 1 red's, green's and blue's factors and the vignetting's gain by distance): kind 0 the
// vignetting undone in light, 1 each pixel from where the lens put it (within the picture, its edge reached).
// The bevels (render/BevelFx, their surface made on the CPU from the picture as the step runs, a pixel's ring round
// the frame, in the table's x, its rows read as one run): 78 Edge Bevel (p[1] strength and the light's way across and
// down, p[2] its colour), 79 Layer Bevel (p[1] whether it reaches past the layer, the highlight's and the shadow's
// opacity and the highlight's blend mode; p[2] the shadow's, p[3] the light's direction, p[4] and p[5] the colours).
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D srcTex;  // RGBA32F, premultiplied: what the pass before made
layout(binding = 4) uniform sampler2D orig;    // RGBA32F: the picture as the effect began
layout(binding = 2) uniform sampler2D extra;   // RGBA32F: what an earlier pass kept, where the mode reads one
layout(binding = 5) uniform sampler2D table;   // RGBA32F: the pass's table, rows as its mode says

const float kM1 = 2610.0 / 16384.0, kM2 = 2523.0 / 4096.0 * 128.0, kC1 = 3424.0 / 4096.0, kC2 = 2413.0 / 4096.0 * 32.0,
            kC3 = 2392.0 / 4096.0 * 32.0;
const float kHa = 0.17883277, kHb = 1.0 - 4.0 * 0.17883277, kHc = 0.55991073;
float powPos(float x, float y) { return x > 0.0 ? pow(x, y) : 0.0; }
float toLinearTf(int tf, float v) {
    if (tf == 0) return powPos(v, 2.4);
    if (tf == 1) {
        v = max(v, 0.0);
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    if (tf == 3) {
        const float q = powPos(clamp(v, 0.0, 1.0), 1.0 / kM2);
        return 10000.0 * powPos(max(q - kC1, 0.0) / (kC2 - kC3 * q), 1.0 / kM1) / 203.0;
    }
    if (tf == 4) {
        v = clamp(v, 0.0, 1.0);
        return v <= 0.5 ? v * v / 3.0 : (exp((v - kHc) / kHa) + kHb) / 12.0;
    }
    if (tf == 22) return powPos(v, 563.0 / 256.0);
    return v;
}
float fromLinearTf(int tf, float l) {
    if (tf == 0) return powPos(l, 1.0 / 2.4);
    if (tf == 1) {
        l = max(l, 0.0);
        return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    }
    if (tf == 3) {
        const float y = powPos(clamp(l * 203.0 / 10000.0, 0.0, 1.0), kM1);
        return pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2);
    }
    if (tf == 4) {
        const float e = max(l, 0.0);
        return e <= 1.0 / 12.0 ? sqrt(3.0 * e) : kHa * log(12.0 * e - kHb) + kHc;
    }
    if (tf == 22) return powPos(l, 256.0 / 563.0);
    return l;
}
float tableDecode(int tf, float v) {
    const float s = v < 0.0 ? -1.0 : 1.0;
    v = abs(v);
    if (v >= 1.25) return s * toLinearTf(tf, v);
    const float x = v / 1.25 * 4096.0;
    const int i = min(4095, int(x));
    const float a = texelFetch(table, ivec2(i, 0), 0).x, b = texelFetch(table, ivec2(i + 1, 0), 0).x;
    return s * (a + (b - a) * (x - float(i)));
}
float tableEncode(int tf, float l) {
    const float s = l < 0.0 ? -1.0 : 1.0;
    l = abs(l);
    const float lowest = exp2(-24.0);
    if (l <= lowest) return s * texelFetch(table, ivec2(0, 0), 0).y * l / lowest;
    const float lg = log2(l);
    if (lg >= 8.0) return s * fromLinearTf(tf, l);
    const float x = (lg + 24.0) / 32.0 * 4096.0;
    const int i = min(4095, int(x));
    const float a = texelFetch(table, ivec2(i, 0), 0).y, b = texelFetch(table, ivec2(i + 1, 0), 0).y;
    return s * (a + (b - a) * (x - float(i)));
}
vec4 tableAt(int row, float x, int last) {
    const int i = min(last - 1, int(x));
    const vec4 a = texelFetch(table, ivec2(i, row), 0), b = texelFetch(table, ivec2(i + 1, row), 0);
    return a + (b - a) * (x - float(i));
}
vec4 texelEdge(ivec2 size, int x, int y, int edge) {
    if (edge == 1) {
        x = clamp(x, 0, size.x - 1);
        y = clamp(y, 0, size.y - 1);
    } else if (edge == 2) {
        x -= size.x * int(floor(float(x) / float(size.x)));
        y -= size.y * int(floor(float(y) / float(size.y)));
    } else if (x < 0 || y < 0 || x >= size.x || y >= size.y) {
        return vec4(0.0);
    }
    return texelFetch(srcTex, ivec2(x, y), 0);
}
vec4 sampleEdge(ivec2 size, float x, float y, int edge) {
    x -= 0.5;
    y -= 0.5;
    const int x0 = int(floor(x)), y0 = int(floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    const vec4 p00 = texelEdge(size, x0, y0, edge), p10 = texelEdge(size, x0 + 1, y0, edge);
    const vec4 p01 = texelEdge(size, x0, y0 + 1, edge), p11 = texelEdge(size, x0 + 1, y0 + 1, edge);
    const vec4 top = p00 + (p10 - p00) * fx, bottom = p01 + (p11 - p01) * fx;
    return top + (bottom - top) * fy;
}
vec4 tableTexel(int i) {
    const int w = textureSize(table, 0).x;
    return texelFetch(table, ivec2(i % w, i / w), 0);
}
// A bevel's surface's slope at the frame's pixel xy (half the differences either side), from the table.
vec2 bevelSlope(ivec2 size, ivec2 xy) {
    const int pw = size.x + 2, i = (xy.y + 1) * pw + xy.x + 1;
    return vec2(tableTexel(i + 1).x - tableTexel(i - 1).x, tableTexel(i + pw).x - tableTexel(i - pw).x) * 0.5;
}
float bevelBlend(int mode, float base, float over) {
    if (mode == 1) return 1.0 - (1.0 - base) * (1.0 - over);
    if (mode == 2) return base + over;
    if (mode == 3) return base * over;
    if (mode == 4) return base < 0.5 ? 2.0 * base * over : 1.0 - 2.0 * (1.0 - base) * (1.0 - over);
    return over;
}

void main() {
    const ivec2 xy = ivec2(gl_FragCoord.xy);
    const ivec2 size = textureSize(srcTex, 0);
    const int mode = int(p[0].x);
    const vec4 px = texelFetch(srcTex, xy, 0);
    if (mode == 73) {
        // Magnify: the lens (everything seen from the centre, larger) with a soft or anti-aliased edge, and its ring
        const float dx = float(xy.x) + 0.5 - p[1].x, dy = float(xy.y) + 0.5 - p[1].y, radius = p[1].z, feather = p[2].x, border = p[2].y;
        const float d = p[2].w > 0.5 ? max(abs(dx), abs(dy)) : sqrt(dx * dx + dy * dy);
        const float cover = feather > 0.0 ? clamp((radius - d) / feather, 0.0, 1.0) : clamp(radius - d + 0.5, 0.0, 1.0);
        vec4 c = px;
        if (cover > 0.0) c += (sampleEdge(size, p[1].x + dx / p[1].w, p[1].y + dy / p[1].w, 0) - c) * (cover * p[2].z);
        if (border > 0.0) {
            const float ring = clamp(min(d - radius + 0.5, radius + border - d + 0.5), 0.0, 1.0);
            if (ring > 0.0) {
                const float a = ring * p[2].z;
                c.rgb += (p[3].rgb - c.rgb) * a;
                c.a += (1.0 - c.a) * a;
            }
        }
        fragColor = c;
        return;
    }
    if (mode == 74) {
        const ivec2 from = xy - ivec2(p[1].xy);
        if (from.x < 0 || from.y < 0 || from.x >= size.x || from.y >= size.y) {
            fragColor = vec4(0.0);
            return;
        }
        const float a = texelFetch(srcTex, from, 0).a * p[1].z;
        fragColor = vec4(p[2].rgb * a, a);
        return;
    }
    if (mode == 75) {
        const vec4 o = texelFetch(orig, xy, 0);
        fragColor = o.a <= 0.0 ? px : o + px * (1.0 - o.a);
        return;
    }
    if (mode == 76) {
        fragColor = px;
        const float a = px.a;
        if (a <= 0.0) return;
        const bool strict = p[1].x > 0.5;
        const float rgbLo = strict ? 0.0 : -0.05, rgbHi = strict ? 1.0 : 1.05, yLo = strict ? 0.0 : -0.01, yHi = strict ? 1.0 : 1.03;
        const float knee = p[1].y;
        const float inv = a < 1.0 ? 1.0 / a : 1.0;
        vec3 c = px.rgb * inv;
        const float luma = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        const bool outside = luma < yLo || luma > yHi || any(lessThan(c, vec3(rgbLo))) || any(greaterThan(c, vec3(rgbHi)));
        if (outside && p[1].z > 0.5) {
            const bool stripe = ((xy.x + xy.y) / 4) % 2 == 0;
            fragColor = vec4(stripe ? a : 0.0, 0.0, stripe ? a : 0.0, a);
            return;
        }
        float l2 = luma;
        if (knee <= 0.0) l2 = clamp(luma, yLo, yHi);
        else if (luma > yHi - knee) l2 = yHi - knee + knee * (1.0 - exp(-(luma - (yHi - knee)) / knee));
        else if (luma < yLo + knee) l2 = yLo + knee - knee * (1.0 - exp(-((yLo + knee) - luma) / knee));
        c += l2 - luma;
        float sat = 1.0;
        for (int k = 0; k < 3; ++k) {
            if (c[k] > rgbHi && c[k] > l2) sat = min(sat, (rgbHi - l2) / (c[k] - l2));
            if (c[k] < rgbLo && c[k] < l2) sat = min(sat, (l2 - rgbLo) / (l2 - c[k]));
        }
        sat = clamp(sat, 0.0, 1.0);
        fragColor = vec4(clamp(vec3(l2) + (c - vec3(l2)) * sat, vec3(rgbLo), vec3(rgbHi)) * a, a);
        return;
    }
    if (mode == 77) {
        const float x = float(xy.x) + 0.5, y = float(xy.y) + 0.5, dx = (x - p[1].z) * p[2].x, dy = (y - p[1].w) * p[2].y;
        const vec4 f = tableAt(1, min(sqrt(dx * dx + dy * dy) / p[2].z, 4096.0), 4096);
        if (int(p[1].x) == 0) {
            fragColor = px;
            if (px.a <= 0.0) return;
            const int tf = int(p[1].y);
            for (int k = 0; k < 3; ++k) fragColor[k] = tableEncode(tf, tableDecode(tf, px[k] / px.a) * f.w) * px.a;
            return;
        }
        const vec2 W = vec2(size);
        const vec2 g = vec2(p[1].z, p[1].w) + (vec2(x, y) - vec2(p[1].z, p[1].w)) * f.g;
        if (!(g.x >= 0.0 && g.y >= 0.0 && g.x <= W.x && g.y <= W.y)) fragColor = vec4(0.0);
        else fragColor = sampleEdge(size, clamp(g.x, 0.5, W.x - 0.5), clamp(g.y, 0.5, W.y - 0.5), 0);
        if (p[2].w > 0.5) {
            for (int k = 0; k < 3; k += 2) {
                const vec2 at = vec2(p[1].z, p[1].w) + (vec2(x, y) - vec2(p[1].z, p[1].w)) * f[k];
                vec4 c = vec4(0.0);
                if (at.x >= 0.0 && at.y >= 0.0 && at.x <= W.x && at.y <= W.y)
                    c = sampleEdge(size, clamp(at.x, 0.5, W.x - 0.5), clamp(at.y, 0.5, W.y - 0.5), 0);
                fragColor[k] = c.a > 0.0 && fragColor.a > 0.0 ? c[k] / c.a * fragColor.a : c[k];
            }
        }
        return;
    }
    if (mode == 78) {
        fragColor = px;
        if (px.a <= 0.0) return;
        const vec2 g = bevelSlope(size, xy);
        if (g.x == 0.0 && g.y == 0.0) return;
        const float s = clamp((-g.x * p[1].y - g.y * p[1].z) / sqrt(g.x * g.x + g.y * g.y + 1.0) * 1.4142135623730951, -1.0, 1.0);
        if (s > 0.0) fragColor.rgb += p[2].rgb * (s * p[1].x * px.a);
        else if (s < 0.0) fragColor.rgb *= 1.0 - min(1.0, -s * p[1].x);
        return;
    }
    if (mode == 79) {
        fragColor = px;
        const bool outside = p[1].x > 0.5;
        const float a = clamp(px.a, 0.0, 1.0);
        if (a <= 0.0 && !outside) return;
        const vec2 g = bevelSlope(size, xy);
        if (g.x == 0.0 && g.y == 0.0) return;
        const vec3 l = p[3].xyz;
        const float lit = (-g.x * l.x - g.y * l.y + l.z) / sqrt(g.x * g.x + g.y * g.y + 1.0) - l.z;
        const float s = lit > 0.0 ? min(1.0, lit / (1.0 - l.z)) : max(-1.0, lit / l.z);
        if (s == 0.0) return;
        const float amount = s > 0.0 ? s * p[1].y : -s * p[1].z;
        const vec3 tint = s > 0.0 ? p[4].rgb : p[5].rgb;
        const int blend = int(s > 0.0 ? p[1].w : p[2].x);
        vec3 o;
        for (int c = 0; c < 3; ++c) {
            const float base = a > 0.0 ? px[c] / a : 0.0;
            o[c] = (base + (bevelBlend(blend, base, tint[c]) - base) * amount) * a;
        }
        if (outside && a < 1.0) {
            const float under = amount * (1.0 - a);
            o += tint * under;
            fragColor.a = a + under;
        }
        fragColor.rgb = o;
        return;
    }
    fragColor = px;
}

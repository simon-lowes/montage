#version 440
// One colour step on a layer at its source size, as render/Processing does it on the CPU (straight colour, pixels
// without alpha left alone, premultiplied again after):
//   0  a decoded frame (16-bit RGBA, two RG16 texels a pixel) premultiplied, through the input transform's LUT if any
//   1  a LUT effect (.cube, 3D or 1D), clamped first when baked into another space, mixed by its strength
//   2  Color Correct, 3  Curves, 4  Hue Curves, 5  ASC CDL, 6  Invert, 7  Black & White, 8  Hue/Saturation, 9  Levels
//   10 a grade node's picture (table) mixed with what came into it (srcTex) through the node's mask and mix
// Through the effect's mask where it has one (p[13..15]): its shape drawn on the CPU (maskTex), an HSL qualifier on
// the colour as it comes in, inverted and faded, and the picture mixed with what the effect made by it (all four
// channels, premultiplied, as the CPU mixes them).
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D srcTex;  // RGBA32F, premultiplied
layout(binding = 2) uniform sampler2D src16;   // RG16
layout(binding = 3) uniform sampler3D lut3;    // RGBA32F, red along x
layout(binding = 4) uniform sampler2D table;   // RGBA32F: a 1D LUT, or curve tables (one a channel)
layout(binding = 5) uniform sampler2D maskTex; // R32F: the mask's shape, the layer's size

float clamp01(float v) { return v > 0.0 ? min(v, 1.0) : 0.0; }  // (NaN to 0, as the CPU does)
float luma(vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float smoothstepCpu(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

// p[0]: y LUT size, z 1D, w clamp first; p[1].xyz domain min, p[2].xyz domain max.
vec3 applyLut(vec3 c) {
    if (p[0].w > 0.5) c = vec3(clamp01(c.r), clamp01(c.g), clamp01(c.b));
    const int n = int(p[0].y);
    vec3 u;
    for (int i = 0; i < 3; ++i) {
        const float span = p[2][i] - p[1][i];
        u[i] = clamp01(span > 0.0 ? (c[i] - p[1][i]) / span : c[i]) * float(n - 1);
    }
    if (p[0].z > 0.5) {
        vec3 o;
        for (int k = 0; k < 3; ++k) {
            const int i0 = min(n - 2, int(u[k]));
            const float f = u[k] - float(i0);
            o[k] = texelFetch(table, ivec2(i0, 0), 0)[k] * (1.0 - f) + texelFetch(table, ivec2(i0 + 1, 0), 0)[k] * f;
        }
        return o;
    }
    const int x0 = min(n - 2, int(u.x)), y0 = min(n - 2, int(u.y)), z0 = min(n - 2, int(u.z));
    const float fx = u.x - float(x0), fy = u.y - float(y0), fz = u.z - float(z0);
    const vec3 c000 = texelFetch(lut3, ivec3(x0, y0, z0), 0).rgb, c100 = texelFetch(lut3, ivec3(x0 + 1, y0, z0), 0).rgb;
    const vec3 c010 = texelFetch(lut3, ivec3(x0, y0 + 1, z0), 0).rgb, c110 = texelFetch(lut3, ivec3(x0 + 1, y0 + 1, z0), 0).rgb;
    const vec3 c001 = texelFetch(lut3, ivec3(x0, y0, z0 + 1), 0).rgb, c101 = texelFetch(lut3, ivec3(x0 + 1, y0, z0 + 1), 0).rgb;
    const vec3 c011 = texelFetch(lut3, ivec3(x0, y0 + 1, z0 + 1), 0).rgb, c111 = texelFetch(lut3, ivec3(x0 + 1, y0 + 1, z0 + 1), 0).rgb;
    const vec3 c00 = c000 * (1.0 - fx) + c100 * fx, c10 = c010 * (1.0 - fx) + c110 * fx;
    const vec3 c01 = c001 * (1.0 - fx) + c101 * fx, c11 = c011 * (1.0 - fx) + c111 * fx;
    const vec3 c0 = c00 * (1.0 - fy) + c10 * fy, c1 = c01 * (1.0 - fy) + c11 * fy;
    return c0 * (1.0 - fz) + c1 * fz;
}

// Color Correct. p[1] exposure, contrast, pivot, saturation; p[2] offset, vibrance, tone on; p[3..6] lift, gamma
// (as 2 to the power), gain, white balance; the tone table (1025 over 0..2, rising) in the table's red.
float toneAt(float l) {
    if (l <= 0.0) return l + texelFetch(table, ivec2(0, 0), 0).r;
    if (l >= 2.0) return l - 2.0 + texelFetch(table, ivec2(1024, 0), 0).r;
    const float x = l * 1024.0 / 2.0;
    const int i = min(1023, int(x));
    const float a = texelFetch(table, ivec2(i, 0), 0).r, b = texelFetch(table, ivec2(i + 1, 0), 0).r;
    return a + (b - a) * (x - float(i));
}
vec3 colorCorrect(vec3 rgb) {
    vec3 c;
    for (int i = 0; i < 3; ++i) {
        float v = rgb[i] * p[1].x * p[6][i];
        v = v * p[5][i] + p[2].x;
        v = v + p[3][i] * (1.0 - v);
        v = v > 0.0 ? pow(v, 1.0 / p[4][i]) : v;
        v = (v - p[1].z) * p[1].y + p[1].z;
        c[i] = v;
    }
    float l = luma(c);
    if (p[2].z > 0.5) {
        const float to = toneAt(l);
        const float m = smoothstepCpu(0.02, 0.08, l), k = l > 1e-4 ? min(8.0, to / l) : 1.0;
        c = (c * k) * m + (c + to - l) * (1.0 - m);
        l = to;
    }
    float s = p[1].w;
    if (p[2].y != 0.0) {
        const float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
        const float cur = mx > 1e-4 ? clamp01((mx - mn) / mx) : 0.0;
        s *= max(0.0, 1.0 + p[2].y * (1.0 - cur) * (1.0 - cur));
    }
    return vec3(l) + (c - vec3(l)) * s;
}

// Curves (1024 segments): the master, then each channel's. p[1].x mix.
float curveAt(int ch, int row, float v, int n) {
    const float x = clamp01(v) * float(n);
    const int i = min(n - 1, int(x));
    const float f = x - float(i);
    return texelFetch(table, ivec2(i, row), 0)[ch] * (1.0 - f) + texelFetch(table, ivec2(i + 1, row), 0)[ch] * f;
}
vec3 curves(vec3 c) {
    const vec3 n = vec3(curveAt(1, 0, curveAt(0, 0, c.r, 1024), 1024), curveAt(2, 0, curveAt(0, 0, c.g, 1024), 1024),
                        curveAt(3, 0, curveAt(0, 0, c.b, 1024), 1024));
    return c + (n - c) * p[1].x;
}

// Hue Curves (360 segments): hue against hue, saturation and luma in the first row's channels, luma against
// saturation in its alpha, saturation against saturation in the second row's red. p[1].x mix.
vec3 hueCurves(vec3 c) {
    const float v = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), ch = v - mn;
    if (v <= 0.0) return c;
    const float s = ch / v;
    float h = 0.0;
    if (ch > 1e-6) {
        if (v == c.r) h = mod((c.g - c.b) / ch + 6.0, 6.0);
        else if (v == c.g) h = (c.b - c.r) / ch + 2.0;
        else h = (c.r - c.g) / ch + 4.0;
        h /= 6.0;
    }
    const float l = luma(c);
    float nh = h + (curveAt(0, 0, h, 360) - 0.5);
    nh -= floor(nh);
    const float ns = clamp(s * 2.0 * curveAt(1, 0, h, 360) * 2.0 * curveAt(3, 0, l, 360) * 2.0 * curveAt(0, 1, s, 360), 0.0, 1.0);
    const float nv = v * max(0.0, 1.0 + (curveAt(2, 0, h, 360) - 0.5) * 2.0 * s);
    const float cc = nv * ns, hp = nh * 6.0, xx = cc * (1.0 - abs(mod(hp, 2.0) - 1.0)), m = nv - cc;
    vec3 o;
    const int sector = int(hp) % 6;
    if (sector == 0) o = vec3(cc, xx, 0.0);
    else if (sector == 1) o = vec3(xx, cc, 0.0);
    else if (sector == 2) o = vec3(0.0, cc, xx);
    else if (sector == 3) o = vec3(0.0, xx, cc);
    else if (sector == 4) o = vec3(xx, 0.0, cc);
    else o = vec3(cc, 0.0, xx);
    return c + (o + vec3(m) - c) * p[1].x;
}

// ASC CDL: p[1..3] slope, offset, power (rgb), p[1].w saturation.
vec3 cdl(vec3 c) {
    vec3 v;
    for (int i = 0; i < 3; ++i) {
        const float x = clamp01(c[i] * p[1][i] + p[2][i]);
        v[i] = p[3][i] == 1.0 ? x : pow(x, p[3][i]);
    }
    if (p[1].w != 1.0) {
        const float l = luma(v);
        v = vec3(clamp01(l + p[1].w * (v.r - l)), clamp01(l + p[1].w * (v.g - l)), clamp01(l + p[1].w * (v.b - l)));
    }
    return v;
}

// Hue/Saturation: p[1] cos and sin of the hue turn, saturation, lightness; p[2].x vibrance.
vec3 hueSat(vec3 c) {
    float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    const float i = 0.596 * c.r - 0.274 * c.g - 0.322 * c.b, q = 0.211 * c.r - 0.523 * c.g + 0.312 * c.b;
    float i2 = i * p[1].x - q * p[1].y, q2 = i * p[1].y + q * p[1].x;
    float s = p[1].z;
    if (p[2].x != 0.0) s *= 1.0 + p[2].x * (1.0 - clamp01(sqrt(i2 * i2 + q2 * q2) * 1.6));
    i2 *= s;
    q2 *= s;
    y += p[1].w * (p[1].w > 0.0 ? (1.0 - y) : y);
    return vec3(y + 0.956 * i2 + 0.621 * q2, y - 0.272 * i2 - 0.647 * q2, y - 1.106 * i2 + 1.703 * q2);
}

// Levels: p[1] input black, span, 1 / gamma; p[2] output black and white.
vec3 levels(vec3 c) {
    vec3 o;
    for (int k = 0; k < 3; ++k) {
        const float u = pow(clamp((c[k] - p[1].x) / p[1].y, 0.0, 1.0), p[1].z);
        o[k] = p[2].x + u * (p[2].y - p[2].x);
    }
    return o;
}

// The mask at this pixel (p[12].x: the effect has one): p[13] x shape drawn, y qualifier, z inverted, w opacity; p[14] hue centre and half width
// (degrees), softness, saturation low; p[15] saturation high, luma low and high.
float band(float v, float lo, float hi, float ramp) {
    if (v >= lo && v <= hi) return 1.0;
    const float o = v < lo ? lo - v : v - hi;
    return ramp <= 0.0 ? 0.0 : max(0.0, 1.0 - o / ramp);
}
float maskAt(ivec2 xy, vec3 c, float a) {
    float m = p[13].x > 0.5 ? texelFetch(maskTex, xy, 0).r : 1.0;
    if (p[13].y > 0.5) {
        if (a <= 1e-6) {
            m = 0.0;
        } else {
            const float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), ch = mx - mn;
            const float sat = mx > 1e-6 ? ch / mx : 0.0, l = luma(c);
            float hue = 0.0;
            if (ch > 1e-6) {
                if (mx == c.r) hue = 60.0 * mod((c.g - c.b) / ch + 6.0, 6.0);
                else if (mx == c.g) hue = 60.0 * ((c.b - c.r) / ch + 2.0);
                else hue = 60.0 * ((c.r - c.g) / ch + 4.0);
            }
            float dh = abs(hue - p[14].x);
            dh = min(dh, 360.0 - dh);
            const float soft = p[14].z;
            const float mh = p[14].y >= 180.0 ? 1.0 : band(dh, 0.0, p[14].y, 60.0 * soft + 1e-9);
            m *= mh * band(sat, p[14].w, p[15].x, 0.25 * soft) * band(l, p[15].y, p[15].z, 0.25 * soft);
        }
    }
    return (p[13].z > 0.5 ? 1.0 - m : m) * p[13].w;
}

void main() {
    const ivec2 xy = ivec2(gl_FragCoord.xy);
    const int mode = int(p[0].x);
    if (mode == 0) {
        const vec4 lo = texelFetch(src16, ivec2(xy.x * 2, xy.y), 0), hi = texelFetch(src16, ivec2(xy.x * 2 + 1, xy.y), 0);
        const vec3 s = vec3(lo.r, lo.g, hi.r);
        const float a = hi.g;
        if (p[3].y > 0.5 && a > 0.0) fragColor = vec4(applyLut(s) * a, a);
        else fragColor = vec4(s * a, a);
        return;
    }
    const vec4 px = texelFetch(srcTex, xy, 0);
    const float a = px.a;
    if (mode == 10) {
        const vec3 c = a > 0.0 ? px.rgb * (1.0 / a) : vec3(0.0);
        fragColor = px + (texelFetch(table, xy, 0) - px) * maskAt(xy, c, a);
        return;
    }
    if (a <= 0.0) {
        fragColor = px;
        return;
    }
    const vec3 c = px.rgb * (1.0 / a);
    vec3 o;
    if (mode == 1) o = c + (applyLut(c) - c) * p[3].x;
    else if (mode == 2) o = colorCorrect(c);
    else if (mode == 3) o = curves(c);
    else if (mode == 4) o = hueCurves(c);
    else if (mode == 5) o = cdl(c);
    else if (mode == 6) o = c + (vec3(1.0) - 2.0 * c) * p[1].x;
    else if (mode == 7) o = c + (luma(c) * p[2].xyz - c) * p[1].x;
    else if (mode == 8) o = hueSat(c);
    else o = levels(c);
    const vec4 made = vec4(o * a, a);
    fragColor = p[12].x > 0.5 ? px + (made - px) * maskAt(xy, c, a) : made;
}

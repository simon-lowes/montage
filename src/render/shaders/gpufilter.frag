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
// and the colour tools that work on the working space's light (render/Crosstalk, render/ColorSlice and the HDR Palette),
// each on its own pixel, p[1].x the working space's transfer function (render/ColorSpace's Transfer):
//   13 Crosstalk: the matrix's rows p[2..4]
//   14 Colour Correlation: p[1].yz saturation and where compression starts, p[2] the exposures changed (low, high,
//      fall-off), p[3..5] into the boundary's RGB, p[6..8] back, p[9] the boundary's luminance weights
//   15 HDR Palette: p[1].yzw reference white's light, the black offset and the mix, p[2] the luminance weights; the table's
//      row 1 the brightness out, the saturation and the red and green balance per 1/32 stop, row 2 the blue balance
//   16 Colour Warper (on code values): p[1] the mix and whether brightness is kept; the table's row 0 the mesh's moves
//      (hue, saturation, stops) at spoke * 5 + ring
//   17 Colour Slice: p[1].yzw reference white's light, the vectors in the table and the mix, p[2] saturation, density, depth
//      and highlights protected, p[3] subtractive and through Rec.709, p[4..6] into Rec.709, p[7..9] back, p[10] the skin
//      vector (centre, width, hue, saturation), p[11].x its density; the table's row 1 the vectors (centre, width, hue,
//      saturation), row 2 their densities
//   18 Film Grain (render/VideoFx): p[1] amount, colour, the grid's size; p[2] its scale and the seed's low and high 16 bits
//   19 RGB Mixer: the matrix's rows p[2..4]
//   20 Grain Match (render/GrainMatch's addGrain): p[1].yzw the amount, the power shaping the grain and its scale, p[2..4]
//      the Cholesky factor, p[5] the response, the channels following it (a model's, by bit), PQ's light at 1 and its slope there, p[6..11] each
//      channel's amplitude in the eight bands; the table the grain's fields, a texel to a pixel
// The modes that take light read the working space's transfer tables (render/TransferTables) from the table's row 0
// (decoding in x, encoding in y) where the CPU does.
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D srcTex;  // RGBA32F, premultiplied: what the pass before made
layout(binding = 4) uniform sampler2D orig;    // RGBA32F: the picture as the effect began
layout(binding = 5) uniform sampler2D table;   // RGBA32F: the pass's table, rows as its mode says

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

// C's fmod (the sign of x).
float fmodC(float x, float y) { return x - y * trunc(x / y); }

// ---- The working space's transfer functions (render/ColorSpace's toLinear and fromLinear) for the display curves the
// GPU composites in: 0 BT.1886, 1 sRGB, 2 linear, 3 PQ (203 nits 1), 4 HLG (scene light), 22 a power of 563/256 ----
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
// Exactly, below black mirrored (render/Crosstalk's Linearise: the display curves).
vec3 linearOf(int tf, vec3 c) {
    if (tf == 2) return c;
    vec3 o;
    for (int k = 0; k < 3; ++k) o[k] = c[k] < 0.0 ? -toLinearTf(tf, -c[k]) : toLinearTf(tf, c[k]);
    return o;
}
vec3 codeOf(int tf, vec3 l) {
    if (tf == 2) return l;
    vec3 o;
    for (int k = 0; k < 3; ++k) o[k] = l[k] < 0.0 ? -fromLinearTf(tf, -l[k]) : fromLinearTf(tf, l[k]);
    return o;
}
// Through the tables (render/TransferTables: code values over 0..1.25, light by its logarithm over 2^-24..2^8, beyond
// them the functions, below black mirrored).
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
// A row of the table at x (linear between its entries, the last span carried on).
vec4 tableAt(int row, float x, int last) {
    const int i = min(last - 1, int(x));
    const vec4 a = texelFetch(table, ivec2(i, row), 0), b = texelFetch(table, ivec2(i + 1, row), 0);
    return a + (b - a) * (x - float(i));
}
// HSV's colour from a hue (sixths of a turn), value and chroma, as the CPU's switch makes it.
vec3 fromHue(float hp, float cc) {
    const float xx = cc * (1.0 - abs(fmodC(hp, 2.0) - 1.0));
    const int sector = int(hp) % 6;
    if (sector == 0) return vec3(cc, xx, 0.0);
    if (sector == 1) return vec3(xx, cc, 0.0);
    if (sector == 2) return vec3(0.0, cc, xx);
    if (sector == 3) return vec3(0.0, xx, cc);
    if (sector == 4) return vec3(xx, 0.0, cc);
    return vec3(cc, 0.0, xx);
}
float acesCct(float l) { return l <= 0.0078125 ? 10.5402377416545 * l + 0.0729055341958355 : (log2(l) + 9.72) / 17.52; }
float acesCctToLinear(float c) {
    return c <= 10.5402377416545 * 0.0078125 + 0.0729055341958355 ? (c - 0.0729055341958355) / 10.5402377416545 : exp2(c * 17.52 - 9.72);
}
float cosineWeight(float d, float w) { return d >= w ? 0.0 : 0.5 + 0.5 * cos(3.14159265358979 * d / w); }
float hueDistance(float a, float b) { return abs(fmodC(a - b + 540.0, 360.0) - 180.0); }
vec3 mat3Rows(int at, vec3 v) { return vec3(dot(p[at].xyz, v), dot(p[at + 1].xyz, v), dot(p[at + 2].xyz, v)); }

// render/VideoFx's hash.
uint hashOne(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// Film Grain's grid cell (gx, gy), transparent outside.
vec3 grainCell(int gx, int gy, ivec2 grid, uint seed, bool colour) {
    if (gx < 0 || gy < 0 || gx >= grid.x || gy >= grid.y) return vec3(0.0);
    vec3 n;
    for (int c = 0; c < 3; ++c) {
        const uint cell = uint(gx) * 73856093u ^ uint(gy) * 19349663u ^ uint(colour ? c : 0) * 83492791u;
        n[c] = float(hashOne(seed ^ hashOne(cell)) & 0xffffu) / 65535.0 - 0.5;
    }
    return n;
}

// Grain Match's light (render/GrainMatch's linear and encoded): display curves carried on past their range, mirrored
// below black, HLG's log curve on past 1, PQ straight on at its slope there.
float grainLinear(int tf, float v) {
    if (v < 0.0 && tf != 2) return -toLinearTf(tf, -v);
    if (v > 1.0 && tf == 4) return (exp((v - kHc) / kHa) + kHb) / 12.0;
    if (v > 1.0 && tf == 3) return p[5].z + (v - 1.0) * p[5].w;
    return toLinearTf(tf, v);
}
float grainEncoded(int tf, float l) {
    if (l < 0.0 && tf != 2) return -fromLinearTf(tf, -l);
    if (tf == 3 && l > p[5].z) return 1.0 + (l - p[5].z) / p[5].w;
    return fromLinearTf(tf, l);
}
float grainBand(int c, int i) { return p[6 + 2 * c + i / 4][i % 4]; }
float grainAmplitude(int c, float light) {
    const float x = log2(max(light, 1e-9)) + 7.5, response = p[5].x;
    if (((int(p[5].y) >> c) & 1) != 0) {
        if (x <= 0.0) return grainBand(c, 0) * exp2(response * max(x, -2.0));
        if (x >= 7.0) return grainBand(c, 7) * exp2(response * min(x - 7.0, 24.0));
        const int i = int(x);
        return grainBand(c, i) * pow(grainBand(c, i + 1) / grainBand(c, i), x - float(i));
    }
    if (x <= 0.0) return grainBand(c, 0);
    if (x >= 7.0) return grainBand(c, 7);
    const int i = int(x);
    return grainBand(c, i) + (grainBand(c, i + 1) - grainBand(c, i)) * (x - float(i));
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
    if (mode >= 13 && mode <= 20 && px.a <= 0.0) {
        fragColor = px;  // (no colour to change)
        return;
    }
    const int tf = int(p[1].x);
    if (mode == 13) {
        const float a = px.a;
        const vec3 l = linearOf(tf, px.rgb / a);
        fragColor = vec4(codeOf(tf, mat3Rows(2, l)) * a, a);
        return;
    }
    if (mode == 14) {
        const float a = px.a;
        vec3 rgb = linearOf(tf, px.rgb / a);
        vec3 b = mat3Rows(3, rgb);
        const float Y = dot(p[9].xyz, b);
        if (Y > 1e-12) {
            const float f = (Y - min(b.x, min(b.y, b.z))) / Y;
            if (f > 0.0) {
                float sat = p[1].y;
                if (sat != 1.0) {
                    const float stops = log2(Y / 0.18), low = p[2].x, high = p[2].y, soft = p[2].z;
                    float w = 1.0;
                    if (stops < low) w = soft > 0.0 ? 1.0 - smoothstepCpu(0.0, soft, low - stops) : 0.0;
                    else if (stops > high) w = soft > 0.0 ? 1.0 - smoothstepCpu(0.0, soft, stops - high) : 0.0;
                    sat = 1.0 + (sat - 1.0) * w;
                }
                const float u = sat * f, start = p[1].z;
                const float target = u <= start ? u : start + (1.0 - start) * tanh((u - start) / (1.0 - start));
                b = Y + (b - Y) * (target / f);
                rgb = mat3Rows(6, b);
            }
        }
        fragColor = vec4(codeOf(tf, rgb) * a, a);
        return;
    }
    if (mode == 15) {
        const float a = px.a, toWhite = p[1].y;
        const vec3 code = px.rgb * (1.0 / a), kw = p[2].xyz;
        vec3 lin;
        for (int k = 0; k < 3; ++k) lin[k] = tableDecode(tf, code[k]) * toWhite;
        const float y = kw.x * lin.x + kw.y * lin.y + kw.z * lin.z;
        const float ev = y > 1e-9 ? log2(y / 0.18) : -16.0;
        const float evc = clamp(ev, -16.0, 12.0), x = (evc + 16.0) / 28.0 * 896.0;
        const vec4 at = tableAt(1, x, 896), blue = tableAt(2, x, 896);
        const float gain = exp2(at.x - evc);
        vec3 o = vec3(lin.x * gain * exp2(at.z), lin.y * gain * exp2(at.w), lin.z * gain * exp2(blue.x));
        if (at.y != 1.0) {
            const float y2 = kw.x * o.x + kw.y * o.y + kw.z * o.z;
            o = y2 + (o - y2) * at.y;
        }
        for (int k = 0; k < 3; ++k) o[k] = code[k] + (tableEncode(tf, (o[k] + p[1].z) / toWhite) - code[k]) * p[1].w;
        fragColor = vec4(o * a, a);
        return;
    }
    if (mode == 16) {
        const float a = px.a;
        const vec3 c = px.rgb * (1.0 / a);
        const float v = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), ch = v - mn;
        if (v <= 0.0) {
            fragColor = vec4(c * a, a);
            return;
        }
        const float s = ch / v;
        float h = 0.0;
        if (ch > 1e-6) {
            if (v == c.r) h = fmodC((c.g - c.b) / ch + 6.0, 6.0);
            else if (v == c.g) h = (c.b - c.r) / ch + 2.0;
            else h = (c.r - c.g) / ch + 4.0;
            h /= 6.0;
        }
        // (render/ColorWarp's WarpField: the four mesh points round the colour, blended bilinearly)
        const float A = (h - floor(h)) * 12.0;
        const int i0 = int(floor(A)) % 12, i1 = (i0 + 1) % 12;
        const float fa = A - floor(A);
        const float B = clamp(s, 0.0, 1.0) * 4.0;
        const int j0 = min(int(floor(B)), 3), j1 = j0 + 1;
        const float fb = B - float(j0);
        const vec3 m = texelFetch(table, ivec2(i0 * 5 + j0, 0), 0).xyz * ((1.0 - fa) * (1.0 - fb)) +
                       texelFetch(table, ivec2(i1 * 5 + j0, 0), 0).xyz * (fa * (1.0 - fb)) +
                       texelFetch(table, ivec2(i0 * 5 + j1, 0), 0).xyz * ((1.0 - fa) * fb) +
                       texelFetch(table, ivec2(i1 * 5 + j1, 0), 0).xyz * (fa * fb);
        float nh = h + m.x / 360.0;
        nh -= floor(nh);
        const float ns = clamp(s + m.y, 0.0, 1.0), k = exp2(m.z), nv = v * k, cc = nv * ns;
        vec3 o = fromHue(nh * 6.0, cc) + (nv - cc);
        if (p[1].y > 0.5) {
            const float before = (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) * k, after = 0.2126 * o.r + 0.7152 * o.g + 0.0722 * o.b;
            if (after > 1e-6) o *= before / after;
        }
        fragColor = vec4((c + (o - c) * p[1].x) * a, a);
        return;
    }
    if (mode == 17) {
        const float a = px.a, white = p[1].y;
        const vec3 code = px.rgb / a;
        vec3 lin;
        for (int k = 0; k < 3; ++k) lin[k] = tableDecode(tf, code[k]) * white;
        const float linMax = max(lin.x, max(lin.y, lin.z));
        const bool ref709 = p[3].y > 0.5;
        if (ref709) lin = mat3Rows(4, lin);
        const vec3 c = vec3(acesCct(lin.x), acesCct(lin.y), acesCct(lin.z));
        const float mx = max(c.x, max(c.y, c.z)), mn = min(c.x, min(c.y, c.z)), ch = mx - mn;
        if (ch <= 1e-6) {
            fragColor = px;
            return;
        }
        float h;
        if (mx == c.x) h = fmodC((c.y - c.z) / ch + 6.0, 6.0);
        else if (mx == c.y) h = (c.z - c.x) / ch + 2.0;
        else h = (c.x - c.y) / ch + 4.0;
        h *= 60.0;
        const vec4 skin = p[10];
        const float ws = cosineWeight(hueDistance(h, skin.x), skin.y);
        float hueShift = ws * skin.z, sat = 1.0 + ws * (skin.w - 1.0), density = ws * p[11].x;
        const int n = int(p[1].z);
        for (int i = 0; i < n; ++i) {
            const vec4 v = texelFetch(table, ivec2(i, 1), 0);
            const float w = (1.0 - ws) * cosineWeight(hueDistance(h, v.x), v.y);
            if (w <= 0.0) continue;
            hueShift += w * v.z;
            sat += w * (v.w - 1.0);
            density += w * texelFetch(table, ivec2(i, 2), 0).x;
        }
        sat = max(0.0, sat * p[2].x);
        density += p[2].y;
        if (hueShift == 0.0 && sat == 1.0 && density == 0.0) {
            fragColor = px;
            return;
        }
        vec3 o = c;
        if (hueShift != 0.0) o = fromHue(fmodC(h + hueShift + 720.0, 360.0) / 60.0, ch) + (mx - ch);
        const bool subtractive = p[3].x > 0.5;
        if (subtractive && sat != 1.0)
            for (int k = 0; k < 3; ++k) o[k] = max(mx - (mx - o[k]) * sat, min(o[k], 0.0729055341958355));
        vec3 l = vec3(acesCctToLinear(o.x), acesCctToLinear(o.y), acesCctToLinear(o.z));
        if (!subtractive && sat != 1.0) {
            const float yy = 0.2126 * l.x + 0.7152 * l.y + 0.0722 * l.z;
            l = yy + (l - yy) * sat;
        }
        const float sNow = min(1.0, ch * sat / (4.0 / 17.52));
        if (density != 0.0 && sNow > 0.0) l *= exp2(-1.0 * p[2].z * density * sNow);
        if (ref709) l = mat3Rows(7, l);
        const float amount = p[1].w * (1.0 - p[2].w * smoothstepCpu(0.5, 1.0, linMax));
        vec3 outc;
        for (int k = 0; k < 3; ++k) outc[k] = (code[k] + (tableEncode(tf, l[k] / white) - code[k]) * amount) * a;
        fragColor = vec4(outc, a);
        return;
    }
    if (mode == 18) {
        const ivec2 grid = ivec2(p[1].zw);
        const uint seed = uint(p[2].z) + uint(p[2].w) * 65536u;
        const bool colour = p[1].y > 0.5;
        // (bilinear on the grid, pixel centres at +0.5, transparent outside: render/VideoFx's sample)
        const float gx = (float(xy.x) + 0.5) * p[2].x - 0.5, gy = (float(xy.y) + 0.5) * p[2].y - 0.5;
        const int x0 = int(floor(gx)), y0 = int(floor(gy));
        const float fx = gx - float(x0), fy = gy - float(y0);
        const vec3 n00 = grainCell(x0, y0, grid, seed, colour), n10 = grainCell(x0 + 1, y0, grid, seed, colour);
        const vec3 n01 = grainCell(x0, y0 + 1, grid, seed, colour), n11 = grainCell(x0 + 1, y0 + 1, grid, seed, colour);
        const vec3 top = n00 + (n10 - n00) * fx, bottom = n01 + (n11 - n01) * fx, g = top + (bottom - top) * fy;
        const float l = clamp((0.2126 * px.r + 0.7152 * px.g + 0.0722 * px.b) / px.a, 0.0, 1.0);
        const float k = p[1].x * (0.4 + 2.4 * l * (1.0 - l)) * px.a;
        fragColor = vec4(max(vec3(0.0), px.rgb + g * k), px.a);
        return;
    }
    if (mode == 19) {
        const float a = px.a;
        fragColor = vec4(mat3Rows(2, px.rgb * (1.0 / a)) * a, a);
        return;
    }
    if (mode == 20) {
        const float a = px.a, power = p[1].z, scale = p[1].w;
        const vec3 z = texelFetch(table, xy, 0).xyz;
        vec3 l;
        for (int c = 0; c < 3; ++c) l[c] = grainLinear(tf, px[c] / a);
        const float light = 0.2126 * l.x + 0.7152 * l.y + 0.0722 * l.z;
        vec3 o;
        for (int c = 0; c < 3; ++c) {
            const float zc = dot(p[2 + c].xyz, z);
            const float gn = power == 1.0 ? zc : sign(zc) * pow(abs(zc), power) * scale;
            o[c] = grainEncoded(tf, l[c] + p[1].y * grainAmplitude(c, light) * gn) * a;
        }
        fragColor = vec4(o, a);
        return;
    }
    fragColor = px;
}

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
// and Edge Extend (render/CleanPlate's fillHoles, its pyramid one level a pass, each texel's colour and how much of it is
// known in alpha):
//   21 the picture's solid colours (alpha at least p[1].x), known; 22 a level pulled from the one below (srcTex);
//   23 a level pushed back from the one above (srcTex) into what it was pulled as (extra); 24 a level's cells of colour
//   p[1].x relaxed towards their neighbours; 25 the picture (orig) took the colours pushed out (srcTex) by p[1].y, where
//   it is neither solid nor clear, when anything was known (the top level, `table`)
// and the warps and stylizers of render/StyleFx and render/VideoFx, what is the same over the frame worked out on the CPU:
//   26 a warp, each pixel bilinear from where it says (p[1].x): 0 Wave Warp, 1 Twirl, 2 Spherize, 3 Ripple, 4 Turbulent
//      Displace (its noise made here by the CPU's hash), 5 Motion Tile (wrapped), 6 Camera Shake, 7 Corner Pin (p[2..4] the
//      inverse map's rows, transparent outside)
//   27 Find Edges, 28 Emboss, 29 Halftone, 30 Duotone, 31 VHS (the table each row's shift, tearing and scanline), 32 Tilt-Shift's
//      last step (the picture, orig, towards its blur by the distance from the band, and the saturation)
//   33 Mesh Warp and Grid Warp (render/MeshWarp's drawWarp): the mesh's triangles, listed by the tiles they reach, in the table
// and 34 Depth Map and Depth Fog (each pixel towards the table's colour by its w, from the frame's depth); Light Rays
// (35 the light above the threshold, 36 a pass along the lines to the centre, 37 added to the picture; with Shadows
// Block Rays, 40 the small copy of what blocks them and 41 the light let through) and 38 Lens Flare;
// 39 Radial Blur (render/StyleFx's radialBlur: p[1] the centre, the zoom's share of the way there, the spin in radians);
// and Aperture Glare (render/StyleFx's apertureGlare, its convolution by FFTs in tiles; each texel of a transform holds
// red + i green in xy and blue in zw): 45 the kernel's spectra from the table (M rows of red and green, then M of
// blue), 42 a tile's light above the threshold, shrunk, 43 a stage of a transform, 44 through the kernel, 46 a tile's
// glare added to what the tiles before made, 47 the glare added to the picture.
// Painterly (render/PaintFx): 49 the structure tensor, smoothed by 1's boxes or 48's kernel, 50 its flow; then 52 the
// anisotropic Kuwahara filter (a wide brush on 55 the picture and 51 its squares, each through 1's boxes) or 53 the
// smear along the flow.
// The modes that take light read the working space's transfer tables (render/TransferTables) from the table's row 0
// (decoding in x, encoding in y) where the CPU does.
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D srcTex;  // RGBA32F, premultiplied: what the pass before made
layout(binding = 4) uniform sampler2D orig;    // RGBA32F: the picture as the effect began
layout(binding = 2) uniform sampler2D extra;   // RGBA32F: what an earlier pass kept, where the mode reads one
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

// ---- render/StyleFx's and render/VideoFx's warps and stylizers ----
// The texel at (x, y) with an edge rule: 0 transparent outside, 1 clamped to the picture, 2 wrapped round it.
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
// Bilinear at (x, y) (pixel centres at +0.5) with an edge rule.
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
float lumaOf(vec4 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
uint hashOne32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// render/StyleFx's valueNoise and fbm.
float valueCorner(int a, int b, int c, uint seed) {
    const uint h = hashOne32(uint(a) * 73856093u ^ hashOne32(uint(b) * 19349663u ^ hashOne32(uint(c) * 83492791u ^ seed)));
    return float(h & 0xffffffu) / 8388607.0 - 1.0;
}
float valueNoise(float x, float y, float z, uint seed) {
    const float fx = floor(x), fy = floor(y), fz = floor(z);
    const int ix = int(fx), iy = int(fy), iz = int(fz);
    vec3 u = vec3(x - fx, y - fy, z - fz);
    u = u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    const float x00 = mix(valueCorner(ix, iy, iz, seed), valueCorner(ix + 1, iy, iz, seed), u.x);
    const float x10 = mix(valueCorner(ix, iy + 1, iz, seed), valueCorner(ix + 1, iy + 1, iz, seed), u.x);
    const float x01 = mix(valueCorner(ix, iy, iz + 1, seed), valueCorner(ix + 1, iy, iz + 1, seed), u.x);
    const float x11 = mix(valueCorner(ix, iy + 1, iz + 1, seed), valueCorner(ix + 1, iy + 1, iz + 1, seed), u.x);
    return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}
float fbm(float x, float y, float z, int octaves, uint seed) {
    float sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(x * f, y * f, z * f + float(o) * 17.31, seed + uint(o) * 101u);
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}
float smoothstepFx(float a, float b, float x) {
    const float u = clamp((x - a) / (b - a), 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}
// Halftone's dot: 1 inside it, for the cell round (x, y) on the grid turned by (c, s).
// (a place within 1/1000 of a cell of the screen's lines is taken as on them, as on the CPU: at 45 degrees -x * s + y * c is
// exactly 0 on the diagonal, which fused multiply-adds turn into a tiny value either side of it, and with it the cell)
float onScreenLine(float u) {
    const float k = floor(u + 0.5);
    return abs(u - k) < 1e-3 ? k : u;
}
float halftoneDot(ivec2 size, float x, float y, float c, float s, int channel) {
    const float cell = p[1].x;
    const float u = onScreenLine((x * c + y * s) / cell), v = onScreenLine((-x * s + y * c) / cell);
    const float cu = floor(u) + 0.5, cv = floor(v) + 0.5;
    const vec4 at = sampleEdge(size, (cu * c - cv * s) * cell, (cu * s + cv * c) * cell, 1);
    const float a = at.a > 0.0 ? at.a : 1.0;
    const float tone = clamp((channel < 0 ? lumaOf(at) : at[channel]) / a, 0.0, 1.0);
    const float r = cell * sqrt((1.0 - tone) / 3.14159265358979);
    const float d = length(vec2(u - cu, v - cv)) * cell;
    return smoothstepFx(r + 0.5, r - 0.5, d);
}

// The table's i-th texel, its rows read as one run.
vec4 tableTexel(int i) {
    const int w = textureSize(table, 0).x;
    return texelFetch(table, ivec2(i % w, i / w), 0);
}
// render/MeshWarp's drawWarp sample: bilinear, clamped to the picture's edge.
vec4 warpSample(ivec2 size, float x, float y) {
    x = clamp(x - 0.5, 0.0, float(size.x - 1));
    y = clamp(y - 0.5, 0.0, float(size.y - 1));
    const int x0 = min(int(x), size.x - 1), y0 = min(int(y), size.y - 1), x1 = min(x0 + 1, size.x - 1), y1 = min(y0 + 1, size.y - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const vec4 a = texelFetch(srcTex, ivec2(x0, y0), 0), b = texelFetch(srcTex, ivec2(x1, y0), 0);
    const vec4 c = texelFetch(srcTex, ivec2(x0, y1), 0), e = texelFetch(srcTex, ivec2(x1, y1), 0);
    const vec4 top = a + (b - a) * fx, bottom = c + (e - c) * fx;
    return top + (bottom - top) * fy;
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
    if (mode == 21) {
        fragColor = px.a >= p[1].x ? vec4(px.rgb / px.a, 1.0) : vec4(0.0);
        return;
    }
    if (mode == 22) {
        float sw = 0.0;
        vec3 acc = vec3(0.0);
        int n = 0;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const ivec2 f = ivec2(2 * xy.x + dx, 2 * xy.y + dy);
                if (f.x >= size.x || f.y >= size.y) continue;
                ++n;
                const vec4 v = texelFetch(srcTex, f, 0);
                if (v.a <= 0.0) continue;
                sw += v.a;
                acc += v.a * v.rgb;
            }
        fragColor = vec4(sw > 0.0 ? acc / sw : vec3(0.0), sw / float(max(1, n)));
        return;
    }
    if (mode == 23) {
        const vec4 f = texelFetch(extra, xy, 0);
        if (f.a >= 1.0) {
            fragColor = f;
            return;
        }
        const float cy = clamp((float(xy.y) + 0.5) / 2.0 - 0.5, 0.0, float(size.y - 1));
        const float cx = clamp((float(xy.x) + 0.5) / 2.0 - 0.5, 0.0, float(size.x - 1));
        const int j0 = int(cy), j1 = min(j0 + 1, size.y - 1), i0 = int(cx), i1 = min(i0 + 1, size.x - 1);
        const float b = cy - float(j0), a = cx - float(i0);
        const vec3 up = (texelFetch(srcTex, ivec2(i0, j0), 0).rgb * (1.0 - a) + texelFetch(srcTex, ivec2(i1, j0), 0).rgb * a) * (1.0 - b) +
                        (texelFetch(srcTex, ivec2(i0, j1), 0).rgb * (1.0 - a) + texelFetch(srcTex, ivec2(i1, j1), 0).rgb * a) * b;
        fragColor = vec4(f.a * f.rgb + (1.0 - f.a) * up, f.a);
        return;
    }
    if (mode == 24) {
        if (((xy.x + xy.y) & 1) != int(p[1].x) || px.a >= 1.0) {
            fragColor = px;
            return;
        }
        vec3 sum = vec3(0.0);
        int n = 0;
        if (xy.x > 0) sum += texelFetch(srcTex, xy - ivec2(1, 0), 0).rgb, ++n;
        if (xy.x + 1 < size.x) sum += texelFetch(srcTex, xy + ivec2(1, 0), 0).rgb, ++n;
        if (xy.y > 0) sum += texelFetch(srcTex, xy - ivec2(0, 1), 0).rgb, ++n;
        if (xy.y + 1 < size.y) sum += texelFetch(srcTex, xy + ivec2(0, 1), 0).rgb, ++n;
        fragColor = n == 0 ? px : vec4(sum / float(n), px.a);
        return;
    }
    if (mode == 25) {
        const vec4 o = texelFetch(orig, xy, 0);
        if (o.a >= p[1].x || o.a <= 0.0 || texelFetch(table, ivec2(0, 0), 0).a <= 0.0) {
            fragColor = o;
            return;
        }
        fragColor = vec4(o.rgb + (px.rgb * o.a - o.rgb) * p[1].y, o.a);
        return;
    }
    if (mode == 26) {
        // Each pixel from where the warp says, bilinear (or left as it was where the warp does not reach)
        const int kind = int(p[1].x);
        const float x = float(xy.x) + 0.5, y = float(xy.y) + 0.5;
        float sx = x, sy = y;
        int edge = 1;
        if (kind == 0) {
            const float dx = p[1].y, dy = p[1].z, height = p[1].w;
            const float s = 6.283185307179586 * (x * dx + y * dy) / p[2].x + p[2].y;
            float w = sin(s);
            if (int(p[2].z) == 1) w = asin(clamp(w, -1.0, 1.0)) * 2.0 / 3.14159265358979;
            else if (int(p[2].z) == 2) w = tanh(6.0 * w) / tanh(6.0);
            sx = x + dy * height * w;
            sy = y - dx * height * w;
        } else if (kind >= 1 && kind <= 3) {
            const float cx = p[2].x, cy = p[2].y, R = p[2].z, rx = x - cx, ry = y - cy, r = sqrt(rx * rx + ry * ry);
            if (r >= R || (kind != 1 && r < 1e-9)) {
                fragColor = px;
                return;
            }
            if (kind == 1) {
                const float a = -p[2].w * (1.0 - r / R) * (1.0 - r / R), c = cos(a), s = sin(a);
                sx = cx + rx * c - ry * s;
                sy = cy + rx * s + ry * c;
            } else if (kind == 2) {
                const float k = pow(r / R, p[2].w) * R / r;
                sx = cx + rx * k;
                sy = cy + ry * k;
            } else {
                const float d = p[2].w * sin(6.283185307179586 * r / p[3].x - p[3].y) * (1.0 - r / R);
                sx = cx + rx / r * (r + d);
                sy = cy + ry / r * (r + d);
            }
        } else if (kind == 4) {
            const float u = x / p[2].x, v = y / p[2].x;
            const int octaves = int(p[2].w);
            sx = x + p[2].y * fbm(u, v, p[2].z, octaves, uint(p[3].x) + uint(p[3].y) * 65536u);
            sy = y + p[2].y * fbm(u, v, p[2].z, octaves, uint(p[3].z) + uint(p[3].w) * 65536u);
        } else if (kind == 5) {
            const float W = float(size.x), H = float(size.y), cw = p[2].z, ch = p[2].w;
            const float u = (x - p[2].x - (W - cw) / 2.0) / cw, v = (y - p[2].y - (H - ch) / 2.0) / ch;
            float fu = u - floor(u), fv = v - floor(v);
            if (p[3].x > 0.5 && mod(floor(u), 2.0) > 0.5) fu = 1.0 - fu;
            if (p[3].x > 0.5 && mod(floor(v), 2.0) > 0.5) fv = 1.0 - fv;
            sx = fu * W;
            sy = fv * H;
            edge = 2;
        } else if (kind == 6) {
            const float rx = (x - p[2].x - p[2].z) / p[3].x, ry = (y - p[2].y - p[2].w) / p[3].x;
            sx = p[2].x + rx * p[3].y - ry * p[3].z;
            sy = p[2].y + rx * p[3].z + ry * p[3].y;
        } else if (kind == 7) {
            // (render/VideoFx's Corner Pin: transparent outside the quadrilateral)
            const float w = dot(p[4].xyz, vec3(x, y, 1.0));
            if (abs(w) < 1e-12) {
                fragColor = vec4(0.0);
                return;
            }
            const float u = dot(p[2].xyz, vec3(x, y, 1.0)) / w, v = dot(p[3].xyz, vec3(x, y, 1.0)) / w;
            if (u < 0.0 || v < 0.0 || u > 1.0 || v > 1.0) {
                fragColor = vec4(0.0);
                return;
            }
            sx = u * float(size.x);
            sy = v * float(size.y);
            edge = 0;
        }
        fragColor = sampleEdge(size, sx, sy, edge);
        return;
    }
    if (mode >= 27 && mode <= 30 && px.a <= 0.0) {
        fragColor = px;  // (nothing there to style)
        return;
    }
    if (mode == 27) {
        // Find Edges: Sobel, each colour on its own, on straight colour, edges clamped
        const float a = px.a;
        vec3 o;
        float n[9 * 3];
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const vec4 q = texelFetch(srcTex, clamp(xy + ivec2(i, j), ivec2(0), size - 1), 0);
                for (int c = 0; c < 3; ++c) n[((j + 1) * 3 + i + 1) * 3 + c] = q.a > 0.0 ? q[c] / q.a : 0.0;
            }
        for (int c = 0; c < 3; ++c) {
            const float gx = n[2 * 3 + c] + 2.0 * n[5 * 3 + c] + n[8 * 3 + c] - n[0 * 3 + c] - 2.0 * n[3 * 3 + c] - n[6 * 3 + c];
            const float gy = n[6 * 3 + c] + 2.0 * n[7 * 3 + c] + n[8 * 3 + c] - n[0 * 3 + c] - 2.0 * n[1 * 3 + c] - n[2 * 3 + c];
            float e = min(1.0, sqrt(gx * gx + gy * gy) / 4.0);
            if (p[1].x > 0.5) e = 1.0 - e;
            o[c] = (e + (px[c] / a - e) * p[1].y) * a;
        }
        fragColor = vec4(o, a);
        return;
    }
    if (mode == 28) {
        // Emboss: grey, raised where the picture brightens towards the light (p[1] the light's offset, contrast, blend)
        const float a = px.a, x = float(xy.x) + 0.5, y = float(xy.y) + 0.5;
        const vec4 lit = sampleEdge(size, x + p[1].x, y + p[1].y, 1), shade = sampleEdge(size, x - p[1].x, y - p[1].y, 1);
        const float v = clamp(0.5 + p[1].z * (lumaOf(lit) - lumaOf(shade)), 0.0, 1.0);
        fragColor = vec4((v + (px.rgb / a - v) * p[1].w) * a, a);
        return;
    }
    if (mode == 29) {
        // Halftone: p[1] the cell, colour; p[2] the cos and sin of the cyan and magenta screens, p[3] the yellow's and black's
        const float a = px.a, x = float(xy.x) + 0.5, y = float(xy.y) + 0.5;
        if (p[1].y > 0.5) {
            fragColor = vec4((1.0 - halftoneDot(size, x, y, p[2].x, p[2].y, 0)) * a, (1.0 - halftoneDot(size, x, y, p[2].z, p[2].w, 1)) * a,
                             (1.0 - halftoneDot(size, x, y, p[3].x, p[3].y, 2)) * a, a);
        } else {
            const float v = 1.0 - halftoneDot(size, x, y, p[3].z, p[3].w, -1);
            fragColor = vec4(vec3(v * a), a);
        }
        return;
    }
    if (mode == 30) {
        // Duotone: p[1].xyz the shadows' colour, p[2].xyz the highlights', p[1].w the mix
        const float a = px.a, l = clamp(lumaOf(px) / a, 0.0, 1.0);
        const vec3 v = p[1].xyz + (p[2].xyz - p[1].xyz) * l;
        fragColor = vec4(px.rgb + (v * a - px.rgb) * p[1].w, a);
        return;
    }
    if (mode == 31) {
        // VHS: p[1] amount, bleed, noise; p[2] softness, the seed's low and high 16 bits, the bleed's taps; p[3].x the
        // noise's streak length; the table each row's shift (x), tearing (y) and scanline's brightness (z)
        const vec3 row = texelFetch(table, ivec2(xy.y, 0), 0).xyz;
        const float sx = float(xy.x) + 0.5 - row.x, y = float(xy.y) + 0.5;
        const vec4 here = sampleEdge(size, sx, y, 1);
        const float a = here.a;
        if (a <= 0.0) {
            fragColor = here;
            return;
        }
        const float amount = p[1].x, bleed = p[1].y, soft = p[2].x;
        const vec4 l0 = sampleEdge(size, sx - soft, y, 1), l1 = sampleEdge(size, sx + soft, y, 1);
        float l = (lumaOf(here) * 2.0 + lumaOf(l0) + lumaOf(l1)) / 4.0 / a;
        float cb = 0.0, cr = 0.0, wsum = 0.0;
        const int taps = int(p[2].w);
        for (int k = 0; k <= 2 * taps; ++k) {
            const vec4 q = sampleEdge(size, sx - bleed * float(k) / float(taps), y, 1);
            const float qa = q.a > 0.0 ? q.a : 1.0, ql = lumaOf(q) / qa;
            cb += q.b / qa - ql, cr += q.r / qa - ql, wsum += 1.0;
        }
        cb /= wsum, cr /= wsum;
        const uint seed = uint(p[2].y) + uint(p[2].z) * 65536u;
        const uint h = hashOne32(seed ^ (uint(xy.y) * 2246822519u + uint(xy.x / int(p[3].x))));
        const float n = float(h & 0xffffu) / 65535.0 - 0.5;
        l += n * p[1].z * (0.35 + row.y * 1.5);
        if (row.y > 0.3 && (h >> 16) % 7u == 0u) l = max(l, 0.85);
        l = 0.04 * amount + l * (1.0 - 0.1 * amount);
        const float sat = 1.0 - 0.2 * amount;
        const float r = l + cr * sat, b = l + cb * sat, g = (l - 0.2126 * r - 0.0722 * b) / 0.7152;
        fragColor = vec4(max(0.0, r * row.z) * a, max(0.0, g * row.z) * a, max(0.0, b * row.z) * a, a);
        return;
    }
    if (mode == 32) {
        // Tilt-Shift's last step: the picture (orig) towards its blur (srcTex, when p[3].x) away from the band (p[1] its
        // centre and normal, p[2] the frame's height, where it starts and finishes going soft, the saturation)
        vec4 o = px;
        if (p[3].x > 0.5) {
            const float d = abs((float(xy.x) + 0.5 - p[1].x) * p[1].z + (float(xy.y) + 0.5 - p[1].y) * p[1].w) / p[2].x;
            const float k = smoothstepFx(p[2].y, p[2].z, d);
            const vec4 sharp = texelFetch(orig, xy, 0);
            o = sharp + (px - sharp) * k;
        }
        if (abs(p[2].w - 1.0) > 1e-4) {
            const float l = lumaOf(o);
            o.rgb = max(vec3(0.0), l + (o.rgb - l) * p[2].w);
        }
        fragColor = o;
        return;
    }
    if (mode == 33) {
        // Mesh Warp and Grid Warp (drawWarp): the triangles that may cover the pixel's tile, back to front, each drawing
        // the pixels it owns by the CPU's rule over those before it. p[1] the tile's size, the tiles across, where the
        // triangles start in the table; the table each tile's list (its start and length) at the tile's index, the lists,
        // then each triangle in 6 texels (edges k: A, B and the vertex P they are worked out from; the values at the
        // vertices with which edges' pixels it owns, by bit; its vertices at rest)
        const int tile = int(p[1].x), tris = int(p[1].z);
        const ivec2 tc = xy / tile;
        const vec4 list = tableTexel(tc.y * int(p[1].y) + tc.x);
        const int first = int(list.x), n = int(list.y);
        const float x = float(xy.x) + 0.5, y = float(xy.y) + 0.5;
        vec4 o = vec4(0.0);
        for (int i = 0; i < n; ++i) {
            const int at = tris + int(tableTexel(first + i).x) * 6;
            const vec4 vals = tableTexel(at + 3);
            const int own = int(vals.w);
            vec3 l;
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) {
                const vec4 ed = tableTexel(at + k);
                const float e = ed.x * (x - ed.z) + ed.y * (y - ed.w);
                l[k] = e / vals[k];
                inside = l[k] > 0.0 || (e == 0.0 && ((own >> k) & 1) != 0);
            }
            if (!inside) continue;
            const vec4 r01 = tableTexel(at + 4), r2 = tableTexel(at + 5);
            const vec4 s = warpSample(size, l.x * r01.x + l.y * r01.z + l.z * r2.x, l.x * r01.y + l.y * r01.w + l.z * r2.y);
            o = s + o * (1.0 - s.a);
        }
        fragColor = o;
        return;
    }
    if (mode == 34) {
        // Depth Map and Depth Fog: towards the table's colour (xyz, straight) by its w, both worked out on the CPU from
        // the frame's depth
        const vec4 to = texelFetch(table, xy, 0);
        fragColor = vec4(px.rgb + (to.xyz * px.a - px.rgb) * to.w, px.a);
        return;
    }
    if (mode == 35) {
        // Light Rays' light: what is above the threshold (p[1].x), tinted (p[2].xyz), no coverage of its own
        const float a = px.a;
        const float l = a > 0.0 ? lumaOf(px) / a : 0.0;
        const float k = !isinf(l) && !isnan(l) && l > p[1].x ? (l - p[1].x) / l : 0.0;
        fragColor = vec4(k > 0.0 ? px.rgb * k * p[2].xyz : vec3(0.0), 0.0);
        return;
    }
    if (mode == 36) {
        // One of its passes: eight samples (weights p[2..3]) along the line to the centre (p[1].xy), p[1].z apart,
        // bilinear, transparent outside
        const float x = float(xy.x) + 0.5, y = float(xy.y) + 0.5;
        vec3 acc = vec3(0.0);
        for (int j = 0; j < 8; ++j) {
            const float f = float(j) * p[1].z;
            acc += p[2 + j / 4][j % 4] * bilinear(size, x + (p[1].x - x) * f, y + (p[1].y - y) * f).rgb;
        }
        fragColor = vec4(acc, 0.0);
        return;
    }
    if (mode == 37) {
        // The rays (srcTex) added to the picture (orig) by p[1].x; light shows on transparent areas too. With p[1].y,
        // the rays are the ones kept (extra), and srcTex the light let through to each of the small copy's pixels (40
        // and 41), drawn up between them
        const vec4 o = texelFetch(orig, xy, 0);
        float k = p[1].x;
        vec3 rays = px.rgb;
        if (p[1].y > 0.5) {
            rays = texelFetch(extra, xy, 0).rgb;
            const ivec2 n = textureSize(srcTex, 0), full = textureSize(orig, 0);
            const float fx = clamp((float(xy.x) + 0.5) * float(n.x) / float(full.x) - 0.5, 0.0, float(n.x - 1));
            const float fy = clamp((float(xy.y) + 0.5) * float(n.y) / float(full.y) - 0.5, 0.0, float(n.y - 1));
            const int x0 = int(fx), y0 = int(fy), x1 = min(n.x - 1, x0 + 1), y1 = min(n.y - 1, y0 + 1);
            const float ax = fx - float(x0), ay = fy - float(y0);
            const float t00 = texelFetch(srcTex, ivec2(x0, y0), 0).r, t10 = texelFetch(srcTex, ivec2(x1, y0), 0).r;
            const float t01 = texelFetch(srcTex, ivec2(x0, y1), 0).r, t11 = texelFetch(srcTex, ivec2(x1, y1), 0).r;
            k *= (t00 * (1.0 - ax) + t10 * ax) * (1.0 - ay) + (t01 * (1.0 - ax) + t11 * ax) * ay;
        }
        const vec3 add = rays * k;
        fragColor = vec4(o.rgb + add, min(1.0, o.a + max(add.r, max(add.g, add.b))));
        return;
    }
    if (mode == 40) {
        // Shadows Block Rays' small copy: of the picture's (orig) pixels this one covers (the table's x of the column
        // index: the columns from and to; zw of the row index: the rows), how much light each lets through, by its
        // brightness against the threshold p[1].x, blocked by p[1].y
        const vec4 cols = texelFetch(table, ivec2(xy.x, 0), 0), rows = texelFetch(table, ivec2(xy.y, 0), 0);
        const int ax = int(cols.x), bx = int(cols.y), ay = int(rows.z), by = int(rows.w);
        float sum = 0.0;
        for (int sy = ay; sy < by; ++sy)
            for (int sx = ax; sx < bx; ++sx) {
                const vec4 q = texelFetch(orig, ivec2(sx, sy), 0);
                const float l = q.a > 0.0 ? lumaOf(q) / q.a : 0.0;
                sum += isinf(l) || isnan(l) ? 1.0 : clamp(l / p[1].x, 0.0, 1.0);
            }
        fragColor = vec4(1.0 - p[1].y * (1.0 - sum / float((bx - ax) * (by - ay))), 0.0, 0.0, 0.0);
        return;
    }
    if (mode == 41) {
        // The light let through to each of the small copy's pixels (srcTex, 40) from the centre's side: a step a pixel
        // along p[1].z of the way to the centre (p[1].xy, the small copy's), each blocking by its share (p[1].w per
        // pixel), bilinear between them, until the march leaves the picture
        const ivec2 n = size;
        const float x = float(xy.x) + 0.5, y = float(xy.y) + 0.5, len = p[1].z;
        const float dx = p[1].x - x, dy = p[1].y - y, reach = sqrt(dx * dx + dy * dy) * len;
        const int steps = max(1, int(ceil(reach)));
        float logT = 0.0;
        for (int j = 1; j <= steps; ++j) {
            const float f = len * float(j) / float(steps);
            const float qx = x + dx * f, qy = y + dy * f;
            if (!(qx >= 0.0 && qy >= 0.0 && qx < float(n.x) && qy < float(n.y))) break;
            const float fx = clamp(qx - 0.5, 0.0, float(n.x - 1)), fy = clamp(qy - 0.5, 0.0, float(n.y - 1));
            const int x0 = int(fx), y0 = int(fy), x1 = min(n.x - 1, x0 + 1), y1 = min(n.y - 1, y0 + 1);
            const float ax = fx - float(x0), ay = fy - float(y0);
            const float v = (texelFetch(srcTex, ivec2(x0, y0), 0).r * (1.0 - ax) + texelFetch(srcTex, ivec2(x1, y0), 0).r * ax) * (1.0 - ay) +
                            (texelFetch(srcTex, ivec2(x0, y1), 0).r * (1.0 - ax) + texelFetch(srcTex, ivec2(x1, y1), 0).r * ax) * ay;
            logT += log(max(1e-4, v)) * (reach / float(steps)) * p[1].w;
        }
        fragColor = vec4(exp(logT), 0.0, 0.0, 0.0);
        return;
    }
    if (mode == 39) {
        // Radial Blur: Zoom Blur's and Spin's taps, one every 1.5 px the smear reaches (up to 64), towards the centre and
        // turned either side, bilinear, transparent outside
        const float fx = float(xy.x) + 0.5 - p[1].x, fy = float(xy.y) + 0.5 - p[1].y;
        const float d = sqrt(fx * fx + fy * fy), zr = p[1].z * d, sr = p[1].w * d;
        const int taps = clamp(int(sqrt(zr * zr + sr * sr) / 1.5) + 1, 1, 64);
        vec4 acc = vec4(0.0);
        for (int k = 0; k < taps; ++k) {
            const float f = taps > 1 ? float(k) / float(taps - 1) : 0.0;
            const float sc = 1.0 - p[1].z * f, ang = taps > 1 ? p[1].w * (f - 0.5) : 0.0;
            const float cs = cos(ang), sn = sin(ang);
            acc += bilinear(size, p[1].x + (fx * cs - fy * sn) * sc, p[1].y + (fx * sn + fy * cs) * sc);
        }
        fragColor = acc / float(taps);
        return;
    }
    if (mode == 45) {
        fragColor = texelFetch(table, xy, 0);
        return;
    }
    if (mode == 42) {
        // A tile's light above the threshold (p[1].x) at the glare's size (each its picture's pixels shrunk p[1].y times
        // each way, their mean), from p[1].zw, p[2].xy wide and high; nothing beyond
        const int f = int(p[1].y), tx = int(p[1].z), ty = int(p[1].w);
        if (xy.x >= int(p[2].x) || xy.y >= int(p[2].y)) {
            fragColor = vec4(0.0);
            return;
        }
        const ivec2 full = textureSize(orig, 0);
        const int gx = tx + xy.x, gy = ty + xy.y;
        vec3 sum = vec3(0.0);
        int n = 0;
        for (int y = gy * f; y < min(full.y, (gy + 1) * f); ++y)
            for (int x = gx * f; x < min(full.x, (gx + 1) * f); ++x) {
                const vec4 q = texelFetch(orig, ivec2(x, y), 0);
                const float l = q.a > 0.0 ? lumaOf(q) / q.a : 0.0;
                if (!isinf(l) && !isnan(l) && l > p[1].x) sum += q.rgb * ((l - p[1].x) / l);
                ++n;
            }
        if (f > 1) sum /= float(n);
        fragColor = vec4(sum.r, sum.g, sum.b, 0.0);
        return;
    }
    if (mode == 43) {
        // A stage of a transform, M (p[1].x) along rows (p[1].z 0) or columns (1), forward or inverse (p[1].w): pairs
        // p[1].y apart in the last stage's order, the second turned by its twiddle (the table's), summed and
        // differenced into natural order (Stockham)
        const int n = int(p[1].x), ns = int(p[1].y);
        const bool column = p[1].z > 0.5;
        const int j = column ? xy.y : xy.x;
        const int r = j % (2 * ns), k = r % ns, i = (j / (2 * ns)) * ns + k;
        const vec4 a = texelFetch(srcTex, column ? ivec2(xy.x, i) : ivec2(i, xy.y), 0);
        const vec4 b = texelFetch(srcTex, column ? ivec2(xy.x, i + n / 2) : ivec2(i + n / 2, xy.y), 0);
        vec2 w = texelFetch(table, ivec2(k * (n / (2 * ns)), 0), 0).xy;
        if (p[1].w > 0.5) w.y = -w.y;
        const vec4 v = vec4(b.x * w.x - b.y * w.y, b.x * w.y + b.y * w.x, b.z * w.x - b.w * w.y, b.z * w.y + b.w * w.x);
        fragColor = r < ns ? a + v : a - v;
        return;
    }
    if (mode == 44) {
        // Through the kernel: red and green parted by the transform's symmetry, each by its own spectrum, blue by its
        const int n = int(p[1].x);
        const vec4 x = texelFetch(srcTex, xy, 0), y = texelFetch(srcTex, ivec2((n - xy.x) % n, (n - xy.y) % n), 0);
        const vec2 red = vec2(x.x + y.x, x.y - y.y) * 0.5;               // (X + conj(Y)) / 2
        const vec2 d = vec2(x.x - y.x, x.y + y.y);                       // X - conj(Y)
        const vec2 green = vec2(d.y * -0.5, d.x * 0.5) * -1.0;           // times -i / 2
        const vec4 s01 = texelFetch(table, xy, 0), s2 = texelFetch(table, ivec2(xy.x, xy.y + n), 0);
        const vec2 rs = vec2(red.x * s01.x - red.y * s01.y, red.x * s01.y + red.y * s01.x);
        const vec2 gs = vec2(green.x * s01.z - green.y * s01.w, green.x * s01.w + green.y * s01.z);
        const vec2 bs = vec2(x.z * s2.x - x.w * s2.y, x.z * s2.y + x.w * s2.x);
        fragColor = vec4(rs.x - gs.y, rs.y + gs.x, bs.x, bs.y);  // red + i green
        return;
    }
    if (mode == 46) {
        // A tile's glare (srcTex, unscaled: by p[2].z) added where it reaches (its tile, p[1], and the kernel's radius
        // p[2].x round it) to what the tiles before made (extra, when p[2].w)
        vec4 o = p[2].w > 0.5 ? texelFetch(extra, xy, 0) : vec4(0.0);
        const int x = xy.x - int(p[1].x), y = xy.y - int(p[1].y), r = int(p[2].x), n = int(p[2].y);
        if (x >= -r && y >= -r && x < int(p[1].z) + r && y < int(p[1].w) + r) {
            const vec4 g = texelFetch(srcTex, ivec2((x + n) % n, (y + n) % n), 0);
            o.rgb += vec3(g.x, g.y, g.z) * p[2].z;
        }
        fragColor = o;
        return;
    }
    if (mode == 47) {
        // The glare (srcTex, shrunk p[1].x times: drawn back up between its pixels) added to the picture by p[1].y
        const vec4 o = texelFetch(orig, xy, 0);
        const int f = int(p[1].x);
        vec3 g;
        if (f == 1) {
            g = texelFetch(srcTex, xy, 0).rgb;
        } else {
            const ivec2 n = size;
            const float sx = clamp((float(xy.x) + 0.5) / float(f) - 0.5, 0.0, float(n.x - 1));
            const float sy = clamp((float(xy.y) + 0.5) / float(f) - 0.5, 0.0, float(n.y - 1));
            const int x0 = int(sx), y0 = int(sy), x1 = min(n.x - 1, x0 + 1), y1 = min(n.y - 1, y0 + 1);
            const float fx = sx - float(x0), fy = sy - float(y0);
            g = (texelFetch(srcTex, ivec2(x0, y0), 0).rgb * (1.0 - fx) + texelFetch(srcTex, ivec2(x1, y0), 0).rgb * fx) * (1.0 - fy) +
                (texelFetch(srcTex, ivec2(x0, y1), 0).rgb * (1.0 - fx) + texelFetch(srcTex, ivec2(x1, y1), 0).rgb * fx) * fy;
        }
        const vec3 add = max(vec3(0.0), g) * p[1].y;
        fragColor = vec4(o.rgb + add, min(1.0, o.a + max(add.r, max(add.g, add.b))));
        return;
    }
    if (mode == 48) {
        // a Gaussian's kernel along a row (p[1].y 0) or down a column (1), its p[1].x + 1 weights from the centre in
        // p[2..], edges held (render/PaintFx's gaussian)
        const int r = int(p[1].x);
        const bool column = p[1].y > 0.5;
        const int n = column ? size.y : size.x, at = column ? xy.y : xy.x;
        vec4 acc = vec4(0.0);
        for (int i = -r; i <= r; ++i) {
            const int k = clamp(at + i, 0, n - 1), a = abs(i);
            acc += p[2 + a / 4][a % 4] * texelFetch(srcTex, column ? ivec2(xy.x, k) : ivec2(k, xy.y), 0);
        }
        fragColor = acc;
        return;
    }
    if (mode == 49) {
        // the structure tensor of the picture's luma (Sobel, edges held): gx^2, gx gy, gy^2
        const ivec2 hi = size - ivec2(1);
        float L[9];
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const vec4 c = texelFetch(srcTex, clamp(xy + ivec2(i, j), ivec2(0), hi), 0);
                L[(j + 1) * 3 + i + 1] = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
            }
        const float gx = ((L[2] + 2.0 * L[5] + L[8]) - (L[0] + 2.0 * L[3] + L[6])) / 4.0;
        const float gy = ((L[6] + 2.0 * L[7] + L[8]) - (L[0] + 2.0 * L[1] + L[2])) / 4.0;
        fragColor = vec4(gx * gx, gx * gy, gy * gy, 0.0);
        return;
    }
    if (mode == 50) {
        // the flow from the smoothed tensor: the unit tangent (across the gradient) and the anisotropy; the tangent
        // from whichever of its two equal forms keeps clear of cancellation
        const float E = px.x, F = px.y, G = px.z;
        const float root = sqrt((E - G) * (E - G) + 4.0 * F * F);
        vec2 t;
        if (E > G) t = vec2(abs(F), -(((E - G) + root) / 2.0) * sign(F));  // (l1 - G, without the cancellation)
        else t = vec2(((G - E) + root) / 2.0, -F);                         // (l1 - E, likewise)
        const float len = sqrt(t.x * t.x + t.y * t.y);
        if (len > 1e-12) t /= len;
        else t = E >= G && E > 0.0 ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
        fragColor = vec4(t, E + G > 1e-12 ? root / (E + G) : 0.0, 0.0);
        return;
    }
    if (mode == 51) {
        // the picture's squares (for a wide brush's box of them), 55 the picture as it is
        const vec4 c = texelFetch(orig, xy, 0);
        fragColor = vec4(c.rgb * c.rgb, 0.0);
        return;
    }
    if (mode == 55) {
        fragColor = texelFetch(orig, xy, 0);
        return;
    }
    if (mode == 52) {
        // the anisotropic Kuwahara filter (render/PaintFx's anisotropicKuwahara): p[1] the radius, alpha, q and the
        // step; the flow in extra, the picture (orig) or at a step past 1 its box (srcTex) and its squares' (table);
        // p[2].x the mix with the picture
        const float radius = p[1].x, alpha = p[1].y, q = p[1].z;
        const int step = int(p[1].w);
        const vec4 fl = texelFetch(extra, xy, 0);
        const float A = fl.z;
        const float a = radius * clamp((alpha + A) / alpha, 0.1, 2.0), b = radius * clamp(alpha / (alpha + A), 0.1, 2.0);
        const float c = fl.x, sn = fl.y;
        const int maxX = int(sqrt(a * a * c * c + b * b * sn * sn)) / step * step;
        const int maxY = int(sqrt(a * a * sn * sn + b * b * c * c)) / step * step;
        vec4 m[8];
        vec3 sq[8];
        float mw[8];
        for (int s = 0; s < 8; ++s) m[s] = vec4(0.0), sq[s] = vec3(0.0), mw[s] = 0.0;
        const ivec2 hi = size - ivec2(1);
        const float h = sqrt(0.5);
        for (int j = -maxY; j <= maxY; j += step)
            for (int k = -maxX; k <= maxX; k += step) {
                float vx = (c * float(k) + sn * float(j)) * 0.5 / a, vy = (-sn * float(k) + c * float(j)) * 0.5 / b;
                const float d2 = vx * vx + vy * vy;
                if (d2 > 0.25) continue;
                const ivec2 at = clamp(xy + ivec2(k, j), ivec2(0), hi);
                const vec4 pc = step > 1 ? texelFetch(srcTex, at, 0) : texelFetch(orig, at, 0);
                const vec3 p2 = step > 1 ? texelFetch(table, at, 0).rgb : pc.rgb * pc.rgb;
                float w[8], n = 0.0;
                for (int hf = 0; hf < 2; ++hf) {
                    if (hf == 1) {
                        const float rx = h * (vx - vy), ry = h * (vx + vy);
                        vx = rx, vy = ry;
                    }
                    const float xx = 0.33 - 0.84 * vx * vx, yy = 0.33 - 0.84 * vy * vy;
                    float z;
                    z = max(0.0, vy + xx), w[hf] = z * z, n += w[hf];
                    z = max(0.0, -vx + yy), w[2 + hf] = z * z, n += w[2 + hf];
                    z = max(0.0, -vy + xx), w[4 + hf] = z * z, n += w[4 + hf];
                    z = max(0.0, vx + yy), w[6 + hf] = z * z, n += w[6 + hf];
                }
                if (n <= 0.0) continue;
                const float g = exp(-3.125 * d2) / n;
                for (int s = 0; s < 8; ++s) {
                    const float ws = w[s] * g;
                    m[s] += ws * pc;
                    sq[s] += ws * p2;
                    mw[s] += ws;
                }
            }
        vec4 outc = vec4(0.0);
        float total = 0.0;
        for (int s = 0; s < 8; ++s) {
            const float ws = mw[s];
            if (ws <= 0.0) continue;
            const vec3 mean = m[s].rgb / ws, v = abs(sq[s] / ws - mean * mean);
            const float sigma2 = v.r + v.g + v.b;
            const float weight = 1.0 / (1.0 + (sigma2 > 0.0 ? pow(255.0 * sigma2, 0.5 * q) : 0.0));
            outc += weight * (m[s] / ws);
            total += weight;
        }
        const vec4 before = texelFetch(orig, xy, 0);
        const vec4 painted = total > 0.0 ? outc / total : before;
        fragColor = before + (painted - before) * p[2].x;
        return;
    }
    if (mode == 53) {
        // the smear along the flow (render/PaintFx's contourSmear): p[1] the steps each way, the Gaussian's sigma along
        // them, the levels and the mix; the flow in srcTex, the picture in orig. Positions are kept as offsets from the
        // pixel, so they stay as exact as the CPU's.
        const int steps = int(p[1].x), levels = int(p[1].z);
        const float sigma = p[1].y;
        const ivec2 hi = size - ivec2(1);
        vec4 acc = texelFetch(orig, xy, 0);
        float total = 1.0;
        for (int dir = -1; dir <= 1; dir += 2) {
            vec2 o = vec2(0.5);  // (the position less the pixel's corner)
            vec2 l = texelFetch(srcTex, xy, 0).xy * float(dir);
            for (int k = 1; k <= steps; ++k) {
                const vec2 mid = o + l * 0.5;
                vec2 t = texelFetch(srcTex, clamp(xy + ivec2(floor(mid)), ivec2(0), hi), 0).xy;
                if (t.x * l.x + t.y * l.y < 0.0) t = -t;
                o += t;
                l = t;
                const vec2 at = vec2(xy) + o;
                if (at.x < 0.0 || at.y < 0.0 || at.x >= float(size.x) || at.y >= float(size.y)) break;
                const float w = exp(-0.5 * float(k * k) / (sigma * sigma));
                // (bilinear, edges held)
                const vec2 f = o - 0.5, fl = floor(f), fr = f - fl;
                const ivec2 b = xy + ivec2(fl);
                const vec4 p00 = texelFetch(orig, clamp(b, ivec2(0), hi), 0), p10 = texelFetch(orig, clamp(b + ivec2(1, 0), ivec2(0), hi), 0);
                const vec4 p01 = texelFetch(orig, clamp(b + ivec2(0, 1), ivec2(0), hi), 0), p11 = texelFetch(orig, clamp(b + ivec2(1, 1), ivec2(0), hi), 0);
                acc += w * (p00 * ((1.0 - fr.x) * (1.0 - fr.y)) + p10 * (fr.x * (1.0 - fr.y)) + p01 * ((1.0 - fr.x) * fr.y) + p11 * (fr.x * fr.y));
                total += w;
            }
        }
        vec4 d = acc / total;
        if (levels >= 2 && d.a > 0.0) {
            const float n = float(levels - 1);
            for (int ch = 0; ch < 3; ++ch) d[ch] = floor(clamp(d[ch] / d.a, 0.0, 1.0) * n + 0.5) / n * d.a;  // (std::round, at or above 0)
        }
        const vec4 before = texelFetch(orig, xy, 0);
        fragColor = before + (d - before) * p[1].w;
        return;
    }
    if (mode == 38) {
        // Lens Flare (render/StyleFx's lensFlare): p[1] the light's place, half the diagonal, the lens; p[2] the frame's
        // centre, brightness, ghosts; p[3] the tint and the star's spokes; the table each ghost (where along the line, its
        // radius and strength; its colour)
        const int lens = int(p[1].w), spokes = int(p[3].w);
        const float R = p[1].z, x = float(xy.x) + 0.5, y = float(xy.y) + 0.5;
        const float dx = x - p[1].x, dy = y - p[1].y, d = sqrt(dx * dx + dy * dy) / R;
        const vec3 tint = p[3].xyz;
        vec3 light = vec3(0.0);
        const float core = 1.6 * exp(-(d / 0.025) * (d / 0.025)) + 0.35 * exp(-d / 0.1);
        for (int c = 0; c < 3; ++c) light[c] += core * (c == 0 ? 1.0 : 0.5 + 0.5 * tint[c] / max(0.01, tint[0]));
        if (lens == 3) {
            const float streak = exp(-(dy / (0.006 * R)) * (dy / (0.006 * R))) * exp(-abs(dx) / (0.8 * R));
            light += vec3(0.25, 0.45, 0.9) * streak;
        } else {
            const float a = atan(dy, dx);
            const float ray = pow(abs(cos(float(spokes) * 0.5 * a)), lens == 0 ? 60.0 : 120.0) * exp(-d / (lens == 0 ? 0.25 : 0.35));
            light += 0.5 * ray;
        }
        if (lens == 0)
            for (int c = 0; c < 3; ++c) {
                const float ring = (d - (0.33 + 0.012 * float(c))) / 0.012;
                light[c] += 0.07 * exp(-ring * ring);
            }
        const int n = textureSize(table, 0).x / 2;
        for (int k = 0; k < n; ++k) {
            const vec4 g = texelFetch(table, ivec2(2 * k, 0), 0), rgb = texelFetch(table, ivec2(2 * k + 1, 0), 0);
            const float gx = p[1].x + (p[2].x - p[1].x) * g.x, gy = p[1].y + (p[2].y - p[1].y) * g.x;
            const float ex = (x - gx) / (lens == 3 ? 1.6 : 1.0), ey = y - gy;
            const float gd = sqrt(ex * ex + ey * ey) / R / g.y;
            if (gd >= 1.15) continue;
            const float disc = (1.0 - clamp((gd - 0.85) / 0.3, 0.0, 1.0)) * (0.7 + 0.3 * min(1.0, gd));
            light += p[2].w * g.z * disc * rgb.rgb;
        }
        const vec3 add = p[2].z * light * tint;
        fragColor = vec4(px.rgb + add, min(1.0, px.a + max(add.r, max(add.g, add.b))));
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

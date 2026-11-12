#version 440
// Two pictures at the canvas's size mixed into one, as render/Processing's transitionMix and render/Compositor's
// applyTrackMatte do it on the CPU (premultiplied):
//   0 to 17: a transition, outgoing `aTex` into incoming `bTex` (p[0].x: 0 Cross Dissolve, 1 Dip to Black, 2 Dip to
//   White, 3 Wipe, 4 Iris, 5 Push, 6 Slide, 7 Zoom, 8 Whip Pan, 9 Zoom Blur, 10 Spin, 11 Glitch, 12 Light Leak,
//   13 Luma Wipe, 14 Shape Wipe, 15 Cube, 16 Flip, 17 Clock Wipe), with what render/Processing's gpuTransitionParams
//   works out for it: p[0] y position, z softness, w direction; p[1] angle, strength, turn, brights first; p[2] the
//   leak's colour and the glitch's seed; p[3] shape, count, cell, reach; p[4] the shape's cos and sin, 3D under way;
//   p[5..7] and p[8..10] the 3D faces' inverse projections (nine each) and whether each faces the camera
//   18: Smooth Cut, A's first frame morphed into B's last along their flow (render/Retime's interpolateFrames): p[0].y
//   the position, p[1] the frames' pixels to the flow's, the flow's grid spacing and its size; `table` its moves
//   30: a track matte, `aTex` through `bTex`'s alpha (or luma, p[0].y), or the reverse (p[0].z)
//   31: Displace by Layer (render/PaintFx's displaceByMap), `aTex` moved by `bTex`'s channels: p[0].yzw the channels
//   across and down and the edges, p[1] the amounts, bicubic and turned, p[2] the picture's own pixels (left, top,
//   right, bottom); turned, p[3] its origin and across, p[4] down, the determinant's sign and size, p[5].xy how far
//   to keep within its edges; p[5].zw whether it moves across and down
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D aTex;  // RGBA32F, premultiplied
layout(binding = 4) uniform sampler2D bTex;
layout(binding = 5) uniform sampler2D table;  // RGBA32F: Smooth Cut's flow

const float PI = 3.14159265358979;

float clamp01(float v) { return v > 0.0 ? min(v, 1.0) : 0.0; }
float smoothstepCpu(float e0, float e1, float x) {
    if (e1 == e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

vec4 pix(sampler2D t, ivec2 size, int x, int y) { return x < 0 || y < 0 || x >= size.x || y >= size.y ? vec4(0.0) : texelFetch(t, ivec2(x, y), 0); }
// Bilinear, transparent outside (transitionMix's sample).
vec4 bilinear(sampler2D t, ivec2 size, float x, float y) {
    x -= 0.5;
    y -= 0.5;
    const int x0 = int(floor(x)), y0 = int(floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    return pix(t, size, x0, y0) * ((1.0 - fx) * (1.0 - fy)) + pix(t, size, x0 + 1, y0) * (fx * (1.0 - fy)) +
           pix(t, size, x0, y0 + 1) * ((1.0 - fx) * fy) + pix(t, size, x0 + 1, y0 + 1) * (fx * fy);
}
vec4 pixA(ivec2 size, int x, int y) { return pix(aTex, size, x, y); }
vec4 pixB(ivec2 size, int x, int y) { return pix(bTex, size, x, y); }
vec4 sampleA(ivec2 size, float x, float y) { return bilinear(aTex, size, x, y); }
vec4 sampleB(ivec2 size, float x, float y) { return bilinear(bTex, size, x, y); }
vec4 sampleOf(bool b, ivec2 size, float x, float y) { return b ? bilinear(bTex, size, x, y) : bilinear(aTex, size, x, y); }

float hashf(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x & 0xffffffu) / float(0xffffff);
}

float shapeDistance(int shape, float qx, float qy) {
    const float ax = abs(qx), ay = abs(qy), r = sqrt(qx * qx + qy * qy);
    if (shape == 1) return ax + ay;
    if (shape == 2) return max(ax, ay);
    if ((shape == 3 || shape == 4) && r == 0.0) return 0.0;  // (atan(0, 0) is undefined here; the CPU's is 0, and r / anything 0)
    if (shape == 3) {
        const float t = atan(-qy, qx) - PI / 2.0;
        return r / (0.62 + 0.38 * cos(5.0 * t));
    }
    if (shape == 4) {
        const float t = atan(-qy + 0.35 * r, qx), s = sin(t), c = cos(t);
        const float rho = (2.0 - 2.0 * s + s * sqrt(abs(c)) / (s + 1.4)) / 2.4;
        return r / max(0.05, rho);
    }
    if (shape == 5) return min(max(ax / 0.35, ay), max(ax, ay / 0.35));
    return r;
}

// Zoom Blur and Spin: a picture moved by k (0 at rest, 1 at the cut) and smeared along that motion.
vec4 moved(bool b, ivec2 size, float fxp, float fyp, float k, bool zoom) {
    const float strength = p[1].y, turn = p[1].z, cx = float(size.x) * 0.5, cy = float(size.y) * 0.5;
    const float smear = strength * k;
    const float d = sqrt(fxp * fxp + fyp * fyp);
    const float reach = zoom ? 0.25 * smear * d : 0.075 * smear * abs(turn) * k * d;
    const int taps = clamp(int(reach / 1.5) + 1, 1, 48);
    vec4 o = vec4(0.0);
    for (int t = 0; t < taps; ++t) {
        const float f = taps > 1 ? float(t) / float(taps - 1) : 0.0;
        float sx, sy;
        if (zoom) {
            const float sc = 1.0 + k * (1.0 + strength) * (1.0 + 0.25 * smear * f);
            sx = cx + fxp / sc, sy = cy + fyp / sc;
        } else {
            const float ang = k * 0.5 * turn * (1.0 + 0.15 * smear * f);
            const float sc = 1.0 - 0.15 * k;
            const float cs = cos(-ang), sn = sin(-ang);
            sx = cx + (fxp * cs - fyp * sn) / sc, sy = cy + (fxp * sn + fyp * cs) / sc;
        }
        o += sampleOf(b, size, sx, sy) / float(taps);
    }
    return o;
}

// Bilinear between pixel indices, clamped to the picture (render/Retime's sampleBilinear).
vec4 clampedSample(sampler2D t, ivec2 size, float x, float y) {
    x = clamp(x, 0.0, float(size.x - 1));
    y = clamp(y, 0.0, float(size.y - 1));
    const int x0 = int(x), y0 = int(y), x1 = min(x0 + 1, size.x - 1), y1 = min(y0 + 1, size.y - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const vec4 a = texelFetch(t, ivec2(x0, y0), 0), b = texelFetch(t, ivec2(x1, y0), 0);
    const vec4 c = texelFetch(t, ivec2(x0, y1), 0), d = texelFetch(t, ivec2(x1, y1), 0);
    const vec4 top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

// Smooth Cut: the flow through this pixel (FlowField::at, bilinear on its grid), then A sampled where it was and B
// where it will be.
vec4 smoothCut(ivec2 xy, ivec2 size) {
    const float u = p[0].y, scale = p[1].x, step = p[1].y;
    const int gw = int(p[1].z), gh = int(p[1].w);
    const float gx = clamp(((float(xy.x) + 0.5) / scale - 0.5) / step, 0.0, float(gw - 1));
    const float gy = clamp(((float(xy.y) + 0.5) / scale - 0.5) / step, 0.0, float(gh - 1));
    const int x0 = int(gx), y0 = int(gy), x1 = min(x0 + 1, gw - 1), y1 = min(y0 + 1, gh - 1);
    const float fx = gx - float(x0), fy = gy - float(y0);
    const vec2 a = texelFetch(table, ivec2(x0, y0), 0).xy, b = texelFetch(table, ivec2(x1, y0), 0).xy;
    const vec2 c = texelFetch(table, ivec2(x0, y1), 0).xy, d = texelFetch(table, ivec2(x1, y1), 0).xy;
    const vec2 f = ((a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + d * fx) * fy) * scale;
    const float x = float(xy.x), y = float(xy.y);
    const vec4 pa = clampedSample(aTex, size, x - u * f.x, y - u * f.y);
    const vec4 pb = clampedSample(bTex, size, x + (1.0 - u) * f.x, y + (1.0 - u) * f.y);
    return pa * (1.0 - u) + pb * u;
}

vec4 transition(int type, ivec2 xy, ivec2 size) {
    if (type == 18) return smoothCut(xy, size);
    const int x = xy.x, y = xy.y, w = size.x, h = size.y;
    const float uf = p[0].y, soft = p[0].z, strength = p[1].y;
    const int dir = int(p[0].w);
    const float bell = sin(PI * uf);
    const vec4 pa = pixA(size, x, y), pb = pixB(size, x, y);
    if (type == 1 || type == 2) {
        const float col = type == 2 ? 1.0 : 0.0;
        if (uf < 0.5) {
            const float k = uf * 2.0;
            return vec4(pa.rgb * (1.0 - k) + col * k, pa.a * (1.0 - k) + k);
        }
        const float k = (uf - 0.5) * 2.0;
        return vec4(col * (1.0 - k) + pb.rgb * k, (1.0 - k) + pb.a * k);
    }
    if (type == 3) {
        const float ang = p[1].x;
        const float nx = (float(x) + 0.5) / float(w) - 0.5, ny = (float(y) + 0.5) / float(h) - 0.5;
        const float proj = nx * cos(ang) + ny * sin(ang);
        const float ext = 0.5 * (abs(cos(ang)) + abs(sin(ang))) + soft;
        const float edge = -ext + 2.0 * ext * uf;
        const float mb = 1.0 - smoothstepCpu(edge - soft, edge + soft, proj);
        return pa * (1.0 - mb) + pb * mb;
    }
    if (type == 4) {
        const float nx = (float(x) + 0.5) - float(w) * 0.5, ny = (float(y) + 0.5) - float(h) * 0.5;
        const float maxR = sqrt(float(w) * float(w) * 0.25 + float(h) * float(h) * 0.25);
        const float d = sqrt(nx * nx + ny * ny) / maxR;
        const float r = uf * (1.0 + soft);
        const float mb = 1.0 - smoothstepCpu(r - soft, r, d);
        return pa * (1.0 - mb) + pb * mb;
    }
    if (type == 5 || type == 6) {
        int ox = 0, oy = 0, shiftB = 0;
        if (dir == 0) ox = -int(floor(uf * float(w) + 0.5)), shiftB = w;
        else if (dir == 1) ox = int(floor(uf * float(w) + 0.5)), shiftB = -w;
        else if (dir == 2) oy = -int(floor(uf * float(h) + 0.5)), shiftB = h;
        else oy = int(floor(uf * float(h) + 0.5)), shiftB = -h;
        const bool horiz = dir <= 1;
        const vec4 a = type == 5 ? pixA(size, x - ox, y - oy) : pa;
        const vec4 b = pixB(size, x - ox - (horiz ? shiftB : 0), y - oy - (horiz ? 0 : shiftB));
        return b + a * (1.0 - b.a);
    }
    if (type == 7) {
        const float sa = 1.0 + uf * strength, sb = 1.0 + (1.0 - uf) * strength;
        const float cx = float(w) * 0.5, cy = float(h) * 0.5, fxp = float(x) + 0.5, fyp = float(y) + 0.5;
        const vec4 a = sampleA(size, cx + (fxp - cx) / sa, cy + (fyp - cy) / sa);
        const vec4 b = sampleB(size, cx + (fxp - cx) / sb, cy + (fyp - cy) / sb);
        return a * (1.0 - uf) + b * uf;
    }
    if (type == 8) {
        const bool horiz = dir <= 1;
        const float sign = (dir == 0 || dir == 2) ? -1.0 : 1.0;
        const float span = horiz ? float(w) : float(h);
        const float e = uf * uf * (3.0 - 2.0 * uf);
        const float travel = sign * e * span, blur = strength * 0.25 * span * bell;
        const int taps = clamp(int(blur / 1.5) + 1, 1, 48);
        vec4 sa = vec4(0.0), sb = vec4(0.0);
        for (int k = 0; k < taps; ++k) {
            const float s = taps > 1 ? (float(k) / float(taps - 1) - 0.5) * blur : 0.0;
            const float ax = horiz ? float(x) + 0.5 - travel - s : float(x) + 0.5;
            const float ay = horiz ? float(y) + 0.5 : float(y) + 0.5 - travel - s;
            const float bx = horiz ? ax + sign * span : ax, by = horiz ? ay : ay + sign * span;
            sa += sampleA(size, ax, ay) / float(taps);
            sb += sampleB(size, bx, by) / float(taps);
        }
        return sb + sa * (1.0 - sb.a);
    }
    if (type == 9 || type == 10) {
        const float fxp = float(x) + 0.5 - float(w) * 0.5, fyp = float(y) + 0.5 - float(h) * 0.5;
        const vec4 qa = moved(false, size, fxp, fyp, min(1.0, uf * 2.0), type == 9);
        const vec4 qb = moved(true, size, fxp, fyp, min(1.0, (1.0 - uf) * 2.0), type == 9);
        const float m = smoothstepCpu(0.45, 0.55, uf);
        return qa * (1.0 - m) + qb * m;
    }
    if (type == 11) {
        const uint seed = uint(p[2].w);
        const float amount = strength * (1.0 - abs(2.0 * uf - 1.0));
        const int band = y / max(2, h / 24);
        const float r1 = hashf(uint(band) * 2654435761u ^ seed * 40503u);
        const float r2 = hashf(uint(band) * 97u ^ seed * 2246822519u);
        const float shift = (r1 < amount * 0.6) ? (r2 - 0.5) * 0.25 * float(w) * amount : 0.0;
        const bool flip = hashf(uint(band) * 31u ^ seed * 3266489917u) < amount * 0.35;
        const bool fromB = (uf >= 0.5) != flip;
        const float split = 0.02 * float(w) * amount;
        const vec4 qr = sampleOf(fromB, size, float(x) + 0.5 - shift + split, float(y) + 0.5);
        const vec4 qg = sampleOf(fromB, size, float(x) + 0.5 - shift, float(y) + 0.5);
        const vec4 qb = sampleOf(fromB, size, float(x) + 0.5 - shift - split, float(y) + 0.5);
        return vec4(qr.r, qg.g, qb.b, max(qr.a, max(qg.a, qb.a)));
    }
    if (type == 12) {
        const float m = smoothstepCpu(0.3, 0.7, uf);
        vec4 o = pa * (1.0 - m) + pb * m;
        const float nx = (float(x) + 0.5) / float(w), ny = (float(y) + 0.5) / float(h);
        const float c1 = -0.3 + 1.6 * uf, c2 = 1.2 - 1.4 * uf;
        const float l = strength * bell *
                        (exp(-((nx - c1) * (nx - c1) / 0.08 + (ny - 0.35) * (ny - 0.35) / 0.3)) +
                         0.6 * exp(-((nx - c2) * (nx - c2) / 0.05 + (ny - 0.8) * (ny - 0.8) / 0.15)));
        o.rgb += l * p[2].xyz;
        o.a = min(1.0, o.a + l);
        return o;
    }
    if (type == 13) {
        float la = pa.a > 1e-6 ? (0.2126 * pa.r + 0.7152 * pa.g + 0.0722 * pa.b) / pa.a : 0.0;
        if (p[1].w > 0.5) la = 1.0 - la;
        const float s = max(1e-3, soft), tt = uf * (1.0 + 2.0 * s) - s;
        const float mb = 1.0 - smoothstepCpu(tt, tt + s, la);
        return pa * (1.0 - mb) + pb * mb;
    }
    if (type == 14) {
        const int shape = int(p[3].x), count = int(p[3].y);
        const float cell = p[3].z, reach = p[3].w, rc = p[4].x, rs = p[4].y;
        float qx, qy;
        if (count == 1) {
            qx = (float(x) + 0.5 - float(w) * 0.5) / cell, qy = (float(y) + 0.5 - float(h) * 0.5) / cell;
        } else {
            const float cx = (floor((float(x) + 0.5) / cell) + 0.5) * cell, cy = (floor((float(y) + 0.5) / cell) + 0.5) * cell;
            qx = (float(x) + 0.5 - cx) / (cell / 2.0), qy = (float(y) + 0.5 - cy) / (cell / 2.0);
        }
        const float d = shapeDistance(shape, qx * rc + qy * rs, -qx * rs + qy * rc);
        const float s = max(1e-3, soft) * reach, tt = uf * (reach + s);
        const float mb = 1.0 - smoothstepCpu(tt - s, tt, d);
        return pa * (1.0 - mb) + pb * mb;
    }
    if (type == 15 || type == 16) {
        if (p[4].z < 0.5) return uf <= 0.0 ? pa : pb;
        for (int k = 0; k < 2; ++k) {
            const int b = k == 0 ? 5 : 8;
            if (p[b + 2].y < 0.5) continue;
            const float sx = float(x) + 0.5, sy = float(y) + 0.5;
            const float tw = p[b + 1].z * sx + p[b + 1].w * sy + p[b + 2].x;
            const float tu = (p[b].x * sx + p[b].y * sy + p[b].z) / tw, tv = (p[b].w * sx + p[b + 1].x * sy + p[b + 1].y) / tw;
            if (tu < 0.0 || tu > 1.0 || tv < 0.0 || tv > 1.0) continue;
            return sampleOf(k == 1, size, tu * float(w), tv * float(h));
        }
        return vec4(0.0);
    }
    if (type == 17) {
        const float dx = float(x) + 0.5 - float(w) * 0.5, dy = float(y) + 0.5 - float(h) * 0.5;
        // (atan(0, -0) is undefined here; the CPU's atan2 gives a half turn)
        float ang = dx == 0.0 && dy == 0.0 ? 0.5 : atan(dx, -dy) / (2.0 * PI);
        if (ang < 0.0) ang += 1.0;
        const float s = max(1e-3, soft), tt = uf * (1.0 + s);
        const float mb = 1.0 - smoothstepCpu(tt - s, tt, ang);
        return pa * (1.0 - mb) + pb * mb;
    }
    return pa * (1.0 - uf) + pb * uf;
}

// Displace by Layer (31): render/PaintFx's channelOf, edgeIndex and sampleAt.
float dispChannel(vec4 q, int ch) {
    float v = 0.5;
    if (ch == 4) {
        v = q.a;
    } else if (q.a > 0.0) {
        const float inv = 1.0 / q.a;
        if (ch == 0) v = (0.2126 * q.r + 0.7152 * q.g + 0.0722 * q.b) * inv;
        else if (ch == 1) v = q.r * inv;
        else if (ch == 2) v = q.g * inv;
        else if (ch == 3) v = q.b * inv;
    }
    return isnan(v) ? 0.5 : clamp(v, 0.0, 1.0);
}
int dispEdge(int i, int lo, int hi, int edge) {
    const int n = hi - lo;
    if (edge == 0) {
        // (wrapped; GLSL's % is undefined for negatives)
        int k = i - lo;
        k -= n * int(floor(float(k) / float(n)));
        if (k < 0) k += n;
        if (k >= n) k -= n;
        return lo + k;
    }
    if (edge == 1) return clamp(i, lo, hi - 1);
    return i < lo || i >= hi ? -1 : i;
}
vec4 dispPixel(ivec4 b, int edge, int xi, int yi, out bool present) {
    const int X = dispEdge(xi, b.x, b.z, edge), Y = dispEdge(yi, b.y, b.w, edge);
    present = X >= 0 && Y >= 0;
    return present ? texelFetch(aTex, ivec2(X, Y), 0) : vec4(0.0);
}
// The picture at (x, y) in pixel indices (centres at whole numbers), bilinear or Catmull-Rom (kept within the four
// nearest pixels, coverage within 0 and 1), outside `b` (x0, y0, x1, y1) as `edge` says.
vec4 dispSample(ivec4 b, int edge, bool bicubic, float x, float y) {
    const int x0 = int(floor(x)), y0 = int(floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    bool present;
    if (!bicubic)
        return dispPixel(b, edge, x0, y0, present) * ((1.0 - fx) * (1.0 - fy)) + dispPixel(b, edge, x0 + 1, y0, present) * (fx * (1.0 - fy)) +
               dispPixel(b, edge, x0, y0 + 1, present) * ((1.0 - fx) * fy) + dispPixel(b, edge, x0 + 1, y0 + 1, present) * (fx * fy);
    float wx[4], wy[4];
    for (int k = 0; k < 2; ++k) {
        const float t = k == 0 ? fx : fy, t2 = t * t, t3 = t2 * t;
        const vec4 w = vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
        for (int i = 0; i < 4; ++i) {
            if (k == 0) wx[i] = w[i];
            else wy[i] = w[i];
        }
    }
    vec4 acc = vec4(0.0);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) acc += dispPixel(b, edge, x0 - 1 + i, y0 - 1 + j, present) * (wx[i] * wy[j]);
    vec4 lo = vec4(0.0), hi = vec4(0.0);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const vec4 q = dispPixel(b, edge, x0 + i, y0 + j, present);
            lo = i + j == 0 ? q : min(lo, q);
            hi = i + j == 0 ? q : max(hi, q);
        }
    acc = clamp(acc, lo, hi);
    acc.a = clamp(acc.a, 0.0, 1.0);
    return acc;
}
vec4 displaced(ivec2 xy, ivec2 size) {
    const vec4 here = texelFetch(aTex, xy, 0);
    const int edge = int(p[0].w);
    const bool bicubic = p[1].z > 0.5, turned = p[1].w > 0.5;
    const float o0 = p[3].x, o1 = p[3].y, a0 = p[3].z, a1 = p[3].w, d0 = p[4].x, d1 = p[4].y, sg = p[4].z, ad = p[4].w;
    // (a point's place in a turned picture's axes, from the cross products worked out as the CPU works them out)
    precise float rx = float(xy.x) + 0.5 - o0, ry = float(xy.y) + 0.5 - o1;
    precise float na = (rx * d1 - ry * d0) * sg, nb = (a0 * ry - a1 * rx) * sg;
    const ivec4 b = turned ? ivec4(0, 0, size) : ivec4(p[2]);
    if (turned ? !(na >= 0.0 && na < ad && nb >= 0.0 && nb < ad) : (xy.x < b.x || xy.y < b.y || xy.x >= b.z || xy.y >= b.w)) return here;
    const vec4 m = texelFetch(bTex, xy, 0);
    const float dx = p[5].z > 0.5 ? p[1].x * (2.0 * dispChannel(m, int(p[0].y)) - 1.0) : 0.0;
    const float dy = p[5].w > 0.5 ? p[1].y * (2.0 * dispChannel(m, int(p[0].z)) - 1.0) : 0.0;
    if (dx == 0.0 && dy == 0.0) return here;
    if (!turned) return dispSample(b, edge, bicubic, float(xy.x) + dx, float(xy.y) + dy);
    precise float mx = float(xy.x) + 0.5 + dx - o0, my = float(xy.y) + 0.5 + dy - o1;
    precise float ma = (mx * d1 - my * d0) * sg, mb = (a0 * my - a1 * mx) * sg;
    if (edge == 2 && !(ma >= 0.0 && ma < ad && mb >= 0.0 && mb < ad)) return vec4(0.0);
    // (where it is moved to, or, wrapped or held, the place within the picture that comes to)
    const float u = ma / ad, v = mb / ad;
    float wu = u, wv = v;
    if (edge == 0) wu -= floor(wu), wv -= floor(wv);
    if (edge != 2) wu = clamp(wu, p[5].x, 1.0 - p[5].x), wv = clamp(wv, p[5].y, 1.0 - p[5].y);
    if (wu == u && wv == v) return dispSample(b, 1, bicubic, float(xy.x) + dx, float(xy.y) + dy);
    return dispSample(b, 1, bicubic, o0 + wu * a0 + wv * d0 - 0.5, o1 + wu * a1 + wv * d1 - 0.5);
}

void main() {
    const ivec2 xy = ivec2(gl_FragCoord.xy);
    const ivec2 size = textureSize(aTex, 0);
    const int mode = int(p[0].x);
    if (mode == 30) {
        const vec4 q = texelFetch(bTex, xy, 0);
        float m = p[0].y > 0.5 ? clamp(0.2126 * q.r + 0.7152 * q.g + 0.0722 * q.b, 0.0, 1.0) : q.a;
        if (p[0].z > 0.5) m = 1.0 - m;
        fragColor = texelFetch(aTex, xy, 0) * m;
        return;
    }
    if (mode == 31) {
        fragColor = displaced(xy, size);
        return;
    }
    fragColor = transition(mode, xy, size);
}

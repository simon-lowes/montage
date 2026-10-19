#version 440
// The finished frame: over black (alpha made whole, as flattenOver does), through the display's LUT when it differs
// from the working space (as convertColor does), and for an 8-bit target rounded as toRgba8 rounds.
// p[0]: x flatten, y through the LUT, z 8-bit, w LUT size; p[1].x a canvas drawn (none: transparent).
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D canvasTex;
layout(binding = 3) uniform sampler3D lut3;

vec3 applyLut(vec3 c) {
    const int n = int(p[0].w);
    const vec3 u = clamp(c, 0.0, 1.0) * float(n - 1);
    const int x0 = min(n - 2, int(u.x)), y0 = min(n - 2, int(u.y)), z0 = min(n - 2, int(u.z));
    const float fx = u.x - float(x0), fy = u.y - float(y0), fz = u.z - float(z0);
    // Tetrahedral (as the CPU's Lut3D::apply): a grey from greys only.
    const ivec3 o = ivec3(x0, y0, z0);
    ivec3 da, db;
    vec4 w;
    if (fx > fy) {
        if (fy > fz) { da = ivec3(1, 0, 0); db = ivec3(1, 1, 0); w = vec4(1.0 - fx, fx - fy, fy - fz, fz); }
        else if (fx > fz) { da = ivec3(1, 0, 0); db = ivec3(1, 0, 1); w = vec4(1.0 - fx, fx - fz, fz - fy, fy); }
        else { da = ivec3(0, 0, 1); db = ivec3(1, 0, 1); w = vec4(1.0 - fz, fz - fx, fx - fy, fy); }
    } else {
        if (fz > fy) { da = ivec3(0, 0, 1); db = ivec3(0, 1, 1); w = vec4(1.0 - fz, fz - fy, fy - fx, fx); }
        else if (fz > fx) { da = ivec3(0, 1, 0); db = ivec3(0, 1, 1); w = vec4(1.0 - fy, fy - fz, fz - fx, fx); }
        else { da = ivec3(0, 1, 0); db = ivec3(1, 1, 0); w = vec4(1.0 - fy, fy - fx, fx - fz, fz); }
    }
    return w.x * texelFetch(lut3, o, 0).rgb + w.y * texelFetch(lut3, o + da, 0).rgb + w.z * texelFetch(lut3, o + db, 0).rgb +
           w.w * texelFetch(lut3, o + ivec3(1, 1, 1), 0).rgb;
}

void main() {
    vec4 c = p[1].x > 0.5 ? texelFetch(canvasTex, ivec2(gl_FragCoord.xy), 0) : vec4(0.0);
    if (p[0].x > 0.5) c.a = 1.0;
    if (p[0].y > 0.5 && c.a > 0.0) c = vec4(applyLut(c.rgb / c.a) * c.a, c.a);
    if (p[0].z > 0.5) {
        if (c.a >= 1.0) {
            c = vec4(floor(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5) / 255.0, 1.0);
        } else {
            const float a = max(c.a, 0.0), inv = a > 1e-6 ? 1.0 / a : 0.0;
            c = vec4(floor(clamp(c.rgb * inv, 0.0, 1.0) * 255.0 + 0.5) / 255.0, floor(a * 255.0 + 0.5) / 255.0);
        }
    }
    fragColor = c;
}

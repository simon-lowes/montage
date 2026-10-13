#version 440
// The finished frame: over black (alpha made whole, as flattenOver does), through the display's LUT when it differs
// from the working space (as convertColor does), and for an 8-bit target rounded as toRgba8 rounds.
// p[0]: x flatten, y through the LUT, z 8-bit, w LUT size.
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
layout(binding = 1) uniform sampler2D canvasTex;
layout(binding = 3) uniform sampler3D lut3;

vec3 applyLut(vec3 c) {
    const int n = int(p[0].w);
    const vec3 u = clamp(c, 0.0, 1.0) * float(n - 1);
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

void main() {
    vec4 c = texelFetch(canvasTex, ivec2(gl_FragCoord.xy), 0);
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

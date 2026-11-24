#version 440
// The draft's points (render/SplatRender's splatsForBlending with points), one instance each in the order they are
// drawn, the nearest last: two triangles over its centre's pixel. p[0].xy the target's size, p[0].z set where row 0 is
// at the top of NDC's y.
layout(location = 0) in vec4 a0;  // centre
layout(location = 2) in vec4 a2;  // colour as drawn, alpha in a
layout(location = 0) flat out vec4 v2;
layout(std140, binding = 0) uniform Params { vec4 p[16]; };
out gl_PerVertex { vec4 gl_Position; };
void main() {
    const int c = gl_VertexIndex;
    const vec2 corner = vec2((c == 1 || c == 2 || c == 4) ? 1.0 : 0.0, (c == 2 || c == 4 || c == 5) ? 1.0 : 0.0);
    const vec2 lo = floor(a0.xy);
    vec2 ndc = mix(lo, lo + 1.0, corner) / p[0].xy * 2.0 - 1.0;
    if (p[0].z > 0.5) ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v2 = a2;
}

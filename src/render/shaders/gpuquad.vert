#version 440
// One triangle over the whole target: every pass works per pixel, addressed by gl_FragCoord (texel row k is
// framebuffer row k on every backend, so nothing is flipped).
out gl_PerVertex { vec4 gl_Position; };
void main() {
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

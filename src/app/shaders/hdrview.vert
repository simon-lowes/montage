#version 440
// A rectangle of the window (in clip space before the backend's correction), with its texture coordinates.
layout(location = 0) in vec2 position;  // 0..1, (0, 0) the top left
layout(location = 0) out vec2 v_uv;
layout(std140, binding = 0) uniform Params {
    mat4 clipCorrection;  // QRhi::clipSpaceCorrMatrix()
    vec4 rect;            // left, top, right, bottom in clip space (y up)
    vec4 tone;            // headroom, content peak, scale, unused
};
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_uv = position;
    gl_Position = clipCorrection * vec4(mix(rect.x, rect.z, position.x), mix(rect.y, rect.w, position.y), 0.0, 1.0);
}

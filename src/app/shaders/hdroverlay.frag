#version 440
// The monitor's own drawing over the picture (guides, handles, labels, the background): premultiplied sRGB, shown at
// SDR white.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params {
    mat4 clipCorrection;
    vec4 rect;
    vec4 tone;
};
layout(binding = 1) uniform sampler2D tex;
void main() {
    vec4 c = texture(tex, v_uv);
    vec3 s = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    vec3 lin = mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), step(0.04045, s));
    fragColor = vec4(lin * c.a * tone.z, c.a);
}

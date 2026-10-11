#version 440
// The picture: linear light relative to SDR white (Rec. 709 primaries, extended range). Brighter than the display's
// headroom can show (when the picture can be), the brightest channel rolls off from 75 % of the headroom, all three
// scaled alike, as render/HdrView does; then the surface's scale (1 on macOS EDR, SDR white / 80 nits for scRGB).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params {
    mat4 clipCorrection;
    vec4 rect;
    vec4 tone;
};
layout(binding = 1) uniform sampler2D tex;
void main() {
    vec3 l = texture(tex, v_uv).rgb;
    float headroom = tone.x;
    if (tone.y > headroom) {
        float mx = max(l.r, max(l.g, l.b));
        float knee = 0.75 * headroom;
        if (mx > knee) {
            float o = knee + (headroom - knee) * (1.0 - exp(-(mx - knee) / (headroom - knee)));
            l *= o / mx;
        }
    }
    fragColor = vec4(l * tone.z, 1.0);
}

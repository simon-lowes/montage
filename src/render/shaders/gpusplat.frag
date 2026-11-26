#version 440
// A draft's point, its colour as it is drawn (render/SplatRender's splatsForBlending with points).
layout(location = 0) flat in vec4 v2;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = v2; }

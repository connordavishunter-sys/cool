#version 460
#include "common.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 w = fd.invViewProj * vec4(vNdc, 1e-4, 1.0);
    vec3 dir = normalize(w.xyz / w.w);
    vec3 c = fd.fog.z > 0.5 ? WATER_FOG * max(fd.sunDir.w, 0.15) : skyColor(dir, true);
    outColor = vec4(c, 1.0);
}

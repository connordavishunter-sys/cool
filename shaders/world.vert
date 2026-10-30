#version 460
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in uint inPacked;
layout(location = 2) in uint inLight;

layout(push_constant) uniform PC { vec4 offset; } pc;

layout(location = 0) out vec3 vPos;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vPacked;
layout(location = 3) out vec4 vLight;
layout(location = 4) out float vAO;
invariant gl_Position;  // the depth prepass and the shading pass must produce identical depth

void main() {
    vPos = inPos + pc.offset.xyz;
    if ((inPacked & (1u << 11)) != 0u) vPos.y += waveHeight(vPos, ((inPacked >> 15) & 1u) != 0u);
    vUV = packedUV(inPacked);
    vPacked = inPacked;
    vLight = unpackUnorm4x8(inLight);
    vAO = float((inPacked >> 13) & 3u) / 3.0;
    gl_Position = fd.viewProj * vec4(vPos, 1.0);
}

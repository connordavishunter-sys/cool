#version 460

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inLayer;
layout(location = 3) in vec4 inColor;

layout(push_constant) uniform PC { vec2 screen; } pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vLayer;
layout(location = 2) out vec4 vColor;

void main() {
    vUV = inUV;
    vLayer = inLayer;
    vColor = vec4(pow(inColor.rgb, vec3(2.2)), inColor.a);
    gl_Position = vec4(inPos / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}

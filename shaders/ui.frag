#version 460

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform sampler2DArray blockTex;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vLayer;
layout(location = 2) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = vLayer < 0.0 ? texture(tex, vUV) : textureLod(blockTex, vec3(vUV, vLayer), 0.0);
    outColor = t * vColor;
}

#version 460
// Depth prepass: only cutout surfaces need the texture, to discard their transparent texels.
layout(constant_id = 0) const int KIND = 0;

layout(set = 0, binding = 1) uniform sampler2DArray blockTex;

layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vPacked;

void main() {
    if (KIND == 1 && texture(blockTex, vec3(vUV, float((vPacked >> 3) & 255u))).a < 0.5) discard;
}

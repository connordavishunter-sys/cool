#version 460

layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D accumTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;

layout(push_constant) uniform PC {
    mat4 invViewProj;
    vec4 selection;  // xyz: selected shape's min corner in render space, w: enabled
    vec4 selSize;    // xyz: its size
    vec4 params;     // x: use accumulation, y: exposure, z: filmic tonemap, w: world size of one pixel at distance 1
    vec4 grade;      // x: brightness gamma; below 1 lifts the darks, above 1 deepens them
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 c = pc.params.x > 0.5 ? texture(accumTex, vUV).rgb : texture(hdrTex, vUV).rgb;
    c *= pc.params.y;
    c = pc.params.z > 0.5 ? aces(c) : clamp(c, 0.0, 1.0);
    c = pow(c, vec3(pc.grade.x));

    // Block selection outline, drawn on the selected shape's visible surfaces near its edges.
    if (pc.selection.w > 0.5) {
        ivec2 size = textureSize(depthTex, 0);
        float d = texelFetch(depthTex, clamp(ivec2(vUV * vec2(size)), ivec2(0), size - 1), 0).r;
        if (d > 0.0) {
            vec4 w = pc.invViewProj * vec4(vUV * 2.0 - 1.0, d, 1.0);
            vec3 p = w.xyz / w.w;
            vec3 l = p - pc.selection.xyz;
            float e = max(length(p) * pc.params.w * 1.5, 0.002);
            if (all(greaterThan(l, vec3(-e))) && all(lessThan(l, pc.selSize.xyz + e))) {
                vec3 m = min(abs(l), abs(pc.selSize.xyz - l));
                int edges = int(m.x < e) + int(m.y < e) + int(m.z < e);
                if (edges >= 2) c = mix(c, vec3(0.0), 0.75);
            }
        }
    }
    outColor = vec4(c, 1.0);
}

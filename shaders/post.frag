#version 460

layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D accumTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D taaTex;
layout(set = 0, binding = 4) uniform sampler2D dlssTex;

layout(push_constant) uniform PC {
    mat4 invViewProj;
    vec4 selection;  // xyz: selected shape's min corner in render space, w: enabled
    vec4 selSize;    // xyz: its size
    vec4 params;     // x: source (0 scene, 1 accumulation, 2 TAA, 3 DLSS), y: exposure, z: filmic tonemap, w: world size of one pixel at distance 1
    vec4 grade;      // x: brightness gamma; below 1 lifts the darks, above 1 deepens them, y: FXAA
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// The final display colour of the image being presented.
vec3 scene(vec2 uv) {
    int s = int(pc.params.x);
    vec3 c = s == 1 ? texture(accumTex, uv).rgb : s == 2 ? texture(taaTex, uv).rgb : s == 3 ? texture(dlssTex, uv).rgb : texture(hdrTex, uv).rgb;
    c *= pc.params.y;
    c = pc.params.z > 0.5 ? aces(c) : clamp(c, 0.0, 1.0);
    return pow(c, vec3(pc.grade.x));
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// FXAA 3.11-style: finds the edge through this pixel, walks along it to both ends, then blends across it.
vec3 fxaa(vec2 uv) {
    vec2 t = 1.0 / vec2(textureSize(hdrTex, 0));
    vec3 cM = scene(uv);
    float lM = luma(cM), lN = luma(scene(uv + vec2(0, -t.y))), lS = luma(scene(uv + vec2(0, t.y)));
    float lW = luma(scene(uv + vec2(-t.x, 0))), lE = luma(scene(uv + vec2(t.x, 0)));
    float lMin = min(lM, min(min(lN, lS), min(lW, lE))), lMax = max(lM, max(max(lN, lS), max(lW, lE))), range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) return cM;

    float lNW = luma(scene(uv - t)), lSE = luma(scene(uv + t)), lNE = luma(scene(uv + vec2(t.x, -t.y))), lSW = luma(scene(uv + vec2(-t.x, t.y)));
    float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    bool horz = edgeH >= edgeV;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE, g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    float step = horz ? t.y : t.x, lEdge, gScaled;
    if (g1 >= g2) { step = -step; lEdge = 0.5 * (l1 + lM); gScaled = 0.25 * g1; }
    else { lEdge = 0.5 * (l2 + lM); gScaled = 0.25 * g2; }

    vec2 e = uv + (horz ? vec2(0, step) : vec2(step, 0)) * 0.5, dir = horz ? vec2(t.x, 0) : vec2(0, t.y);
    vec2 p1 = e - dir, p2 = e + dir;
    float d1 = luma(scene(p1)) - lEdge, d2 = luma(scene(p2)) - lEdge;
    bool done1 = abs(d1) >= gScaled, done2 = abs(d2) >= gScaled;
    const float STEPS[10] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(done1 && done2); ++i) {
        if (!done1) { p1 -= dir * STEPS[i]; d1 = luma(scene(p1)) - lEdge; done1 = abs(d1) >= gScaled; }
        if (!done2) { p2 += dir * STEPS[i]; d2 = luma(scene(p2)) - lEdge; done2 = abs(d2) >= gScaled; }
    }
    float dist1 = horz ? uv.x - p1.x : uv.y - p1.y, dist2 = horz ? p2.x - uv.x : p2.y - uv.y;
    bool near1 = dist1 < dist2;
    float off = 0.5 - min(dist1, dist2) / (dist1 + dist2);
    if (((near1 ? d1 : d2) < 0.0) == (lM < lEdge)) off = 0.0;  // the end found isn't the one this pixel blends towards

    float sub = clamp(abs((2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0 - lM) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    off = max(off, sub * sub * 0.75);
    return scene(uv + (horz ? vec2(0, off * step) : vec2(off * step, 0)));
}

void main() {
    vec3 c = pc.grade.y > 0.5 ? fxaa(vUV) : scene(vUV);

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

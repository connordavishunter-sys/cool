#ifndef COMMON_GLSL
#define COMMON_GLSL

// All positions are in "render space": world space translated so the camera sits at the origin.
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 sunDir;      // xyz: direction towards the sun (or moon at night), w: daylight 0..1
    vec4 sunColor;    // rgb: radiance of the main light
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 fog;         // x: start, y: end, z: underwater flag, w: time in seconds
    uvec4 info;       // x: lighting mode, y: frame index, z: light count
} fd;

const vec3 NORMALS[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
vec2 packedUV(uint p) { return vec2((p >> 18) & 31u, (p >> 23) & 31u) / 16.0; }
const vec3 WATER_FOG = vec3(0.02, 0.09, 0.16);

vec3 skyColor(vec3 dir, bool withSun) {
    float t = dir.y;
    vec3 c = mix(fd.skyHorizon.rgb, fd.skyZenith.rgb, sqrt(clamp(t, 0.0, 1.0)));
    if (t < 0.0) c = mix(fd.skyHorizon.rgb, fd.skyHorizon.rgb * 0.35, min(-t * 3.0, 1.0));
    if (withSun) {
        float d = dot(dir, fd.sunDir.xyz);
        vec3 disc = fd.sunDir.w > 0.0 ? vec3(1.0, 0.95, 0.85) * 30.0 : vec3(0.7, 0.75, 0.9) * 3.0;
        c += disc * smoothstep(0.9991, 0.9994, d);
        c += fd.sunColor.rgb * pow(max(d, 0.0), 48.0) * 0.25;
    }
    return c;
}

#endif

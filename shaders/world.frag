#version 460
#ifdef RT
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif
#include "common.glsl"

layout(constant_id = 0) const int KIND = 0;  // 0 opaque, 1 cutout, 2 water, 3 translucent

layout(set = 0, binding = 1) uniform sampler2DArray blockTex;

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vPacked;
layout(location = 3) in vec4 vLight;
layout(location = 4) in float vAO;

layout(location = 0) out vec4 outColor;

const float EMISSION = 2.5;

// Classic light-level curve, partway between the original "moody" and "bright" settings.
float mcCurve(float l) {
    float b = 1.0 - l;
    return mix(l / (4.0 - 3.0 * l), 1.0 - b * b * b, 0.6);
}

float faceShade(uint n) { return n == 2u ? 1.0 : n == 3u ? 0.5 : n < 2u ? 0.6 : 0.8; }

vec3 classicLight(uint n) {
    float sky = mcCurve(vLight.x) * mix(0.1, 1.0, fd.sunDir.w);
    vec3 blk = vec3(mcCurve(vLight.y), mcCurve(vLight.z), mcCurve(vLight.w)) * 1.15;
    vec3 l = max(max(vec3(sky), blk), vec3(0.025));
    return pow(l * faceShade(n) * mix(0.45, 1.0, vAO), vec3(2.2));
}

#ifdef RT
layout(set = 0, binding = 2) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 3, std430) readonly buffer ChunkTable { uint64_t chunkVerts[]; };
struct Light { vec4 pos; vec4 color; };
layout(set = 0, binding = 4, std430) readonly buffer LightBuf { Light lights[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexData { uint v[]; };

uint rngState;
float rnd() {
    rngState = rngState * 747796405u + 2891336453u;
    uint w = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return float((w >> 22u) ^ w) / 4294967296.0;
}

vec3 cosineDir(vec3 n) {
    float phi = 6.2831853 * rnd(), r2 = rnd(), r = sqrt(r2);
    vec3 t = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 b = cross(n, t);
    return normalize(t * cos(phi) * r + b * sin(phi) * r + n * sqrt(1.0 - r2));
}

// Fetches a triangle's packed data and texture coordinate. Quads are indexed (0,1,2)(0,2,3);
// each vertex is 5 uints with packed data at offset 3. Each BLAS geometry has its own table entry.
uint fetchTri(uint inst, uint geom, uint prim, vec2 b, out vec2 uv) {
    VertexData vd = VertexData(chunkVerts[inst * 3u + geom]);
    uint i0 = (prim >> 1) * 4u;
    uint i1 = i0 + 1u + (prim & 1u);
    uint p0 = vd.v[i0 * 5u + 3u], p1 = vd.v[i1 * 5u + 3u], p2 = vd.v[(i1 + 1u) * 5u + 3u];
    uv = packedUV(p0) * (1.0 - b.x - b.y) + packedUV(p1) * b.x + packedUV(p2) * b.y;
    return p0;
}

// Runs a ray query over the non-opaque geometry: cutout quads are alpha tested so leaves and glass
// cast proper shadows; translucent quads never block the ray but tint what passes through them.
void traverse(rayQueryEXT rq, inout vec3 transmit) {
    while (rayQueryProceedEXT(rq)) {
        vec2 uv;
        uint p = fetchTri(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)), uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)),
                          uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), uv);
        vec4 tex = textureLod(blockTex, vec3(uv, float((p >> 3) & 255u)), 0.0);
        if (((p >> 16) & 3u) == 3u) transmit *= mix(vec3(1.0), tex.rgb, tex.a);
        else if (tex.a >= 0.5) rayQueryConfirmIntersectionEXT(rq);
    }
}

// Fraction of light that reaches o from direction d: zero when blocked, tinted by stained glass.
vec3 visibility(vec3 o, vec3 d, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, o, 0.001, d, tmax);
    vec3 transmit = vec3(1.0);
    traverse(rq, transmit);
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT ? vec3(0.0) : transmit;
}

struct Hit { vec3 pos; vec3 n; vec3 albedo; bool emissive; };

bool trace(vec3 o, vec3 d, float tmax, out Hit h) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoneEXT, 0xFF, o, 0.001, d, tmax);
    vec3 transmit = vec3(1.0);
    traverse(rq, transmit);
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return false;

    vec2 uv;
    uint p0 = fetchTri(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)), uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)),
                       uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), rayQueryGetIntersectionBarycentricsEXT(rq, true), uv);
    h.pos = o + d * rayQueryGetIntersectionTEXT(rq, true);
    h.n = NORMALS[p0 & 7u];
    if (dot(h.n, d) > 0.0) h.n = -h.n;  // cutout faces can be hit from behind
    h.albedo = textureLod(blockTex, vec3(uv, float((p0 >> 3) & 255u)), 1.0).rgb;
    h.emissive = ((p0 >> 15) & 1u) != 0u;
    return true;
}

// Unshadowed contribution of a block light, with a window so it fades out at 16 blocks.
vec3 lightContribution(Light l, vec3 p, vec3 n, out vec3 dir, out float dist) {
    vec3 dv = l.pos.xyz - p;
    float d2 = dot(dv, dv);
    dist = sqrt(d2);
    dir = dv / max(dist, 1e-4);
    float ndl = dot(n, dir);
    if (d2 > 256.0 || ndl <= 0.0) return vec3(0.0);
    float win = 1.0 - d2 / 256.0;
    return l.color.rgb * ndl * win * win / (d2 + 0.5);
}

vec3 sunLight(vec3 o, vec3 n, float jitter) {
    vec3 d = normalize(fd.sunDir.xyz + (vec3(rnd(), rnd(), rnd()) - 0.5) * jitter);
    float ndl = dot(n, d);
    if (ndl <= 0.0) return vec3(0.0);
    return fd.sunColor.rgb * ndl * visibility(o, d, 4096.0);
}

// Ray traced mode: hard-ish shadows from the sun and nearby block lights.
vec3 directRT(vec3 p, vec3 n) {
    vec3 o = p + n * 0.002;
    vec3 c = sunLight(o, n, 0.0);
    int rays = 0;
    for (uint i = 0u; i < fd.info.z; ++i) {
        vec3 dir; float dist;
        vec3 lc = lightContribution(lights[i], p, n, dir, dist);
        if (max(lc.r, max(lc.g, lc.b)) < 0.003) continue;
        if (rays < 8) {
            ++rays;
            lc *= visibility(o, dir, dist - 0.9);
        }
        c += lc;
    }
    return c;
}

// Path traced mode: next-event estimation with one light chosen proportionally to its contribution.
vec3 directPT(vec3 p, vec3 n) {
    vec3 o = p + n * 0.002;
    vec3 c = sunLight(o, n, 0.03);
    float total = 0.0, pick = rnd();
    int chosen = -1;
    float chosenW = 0.0;
    for (uint i = 0u; i < fd.info.z; ++i) {
        vec3 dir; float dist;
        float w = dot(lightContribution(lights[i], p, n, dir, dist), vec3(0.3, 0.6, 0.1));
        if (w <= 0.0) continue;
        total += w;
        if (pick * total < w) { chosen = int(i); chosenW = w; }
    }
    if (chosen >= 0) {
        Light l = lights[chosen];
        l.pos.xyz += (vec3(rnd(), rnd(), rnd()) - 0.5) * 0.9;
        vec3 dir; float dist;
        vec3 lc = lightContribution(l, p, n, dir, dist);
        c += lc * visibility(o, dir, dist - 0.9) * total / chosenW;
    }
    return c;
}

vec3 pathTrace(vec3 p, vec3 n, vec3 albedo) {
    vec3 result = albedo * directPT(p, n);
    vec3 throughput = albedo;
    for (int bounce = 0; bounce < 2; ++bounce) {
        vec3 d = cosineDir(n);
        Hit h;
        if (!trace(p + n * 0.002, d, 512.0, h)) {
            result += throughput * skyColor(d, false);
            break;
        }
        if (h.emissive) break;  // block lights are already sampled explicitly
        throughput *= h.albedo;
        result += throughput * directPT(h.pos, h.n);
        p = h.pos;
        n = h.n;
    }
    return result;
}

vec3 shadeHit(Hit h) {
    if (h.emissive) return h.albedo * EMISSION;
    return h.albedo * (directRT(h.pos, h.n) + fd.skyZenith.rgb * 0.35);
}
#endif

void main() {
    uint n = vPacked & 7u;
    float layer = float((vPacked >> 3) & 255u);
    bool emissive = ((vPacked >> 15) & 1u) != 0u;
    vec2 uv = vUV;
    if (KIND == 2) uv.y -= fract(fd.fog.w * 0.04);
    vec4 tex = texture(blockTex, vec3(uv, layer));
    if (KIND == 1 && tex.a < 0.5) discard;

    vec3 N = NORMALS[n];
    vec3 V = normalize(vPos);
    if (KIND == 2 && dot(V, N) > 0.0) N = -N;  // looking at the surface from below
    uint mode = fd.info.x;
    vec3 col;

#ifdef RT
    rngState = (uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + fd.info.y * 26699u) | 1u;
    rnd();
#endif

    if (emissive) {
        col = tex.rgb * (mode == 0u ? 1.0 : EMISSION);
    } else if (mode == 0u) {
        col = tex.rgb * classicLight(n);
    }
#ifdef RT
    else if (mode == 1u || KIND >= 2) {  // blended surfaces aren't accumulated, so they use direct lighting
        float ambient = mcCurve(vLight.x) * mix(0.45, 1.0, vAO);
        col = tex.rgb * (directRT(vPos, N) + fd.skyZenith.rgb * 0.6 * ambient + 0.01);
    } else {
        col = pathTrace(vPos, N, tex.rgb);
    }
#else
    else {
        col = tex.rgb * classicLight(n);
    }
#endif

    float alpha = KIND >= 2 ? tex.a : 1.0;
#ifdef RT
    if (KIND == 2 && mode != 0u) {
        vec3 R = reflect(V, N);
        float F = 0.02 + 0.98 * pow(1.0 - max(dot(-V, N), 0.0), 5.0);
        Hit h;
        vec3 refl = trace(vPos + N * 0.01, R, 256.0, h) ? shadeHit(h) : skyColor(R, true);
        col = mix(col, refl, F);
        alpha = mix(alpha, 1.0, F);
    }
#endif

    float dist = length(vPos);
    if (fd.fog.z > 0.5) {
        col = mix(col, WATER_FOG * max(fd.sunDir.w, 0.15), 1.0 - exp(-dist * 0.08));
    } else {
        col = mix(col, skyColor(V, false), smoothstep(fd.fog.x, fd.fog.y, dist));
    }
    outColor = vec4(col, alpha);
}

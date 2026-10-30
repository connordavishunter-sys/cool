#include "particles.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr float GRAVITY = 14, DRAG = 1.5f;

bool solidAt(const World& world, glm::dvec3 p) {
    if (p.y < 0 || p.y >= CH) return false;
    return blockInfo(world.get((int)std::floor(p.x), (int)std::floor(p.y), (int)std::floor(p.z))).solid;
}
}  // namespace

void Particles::spawn(const World& world, glm::dvec3 pos, glm::vec3 vel, BlockId block) {
    if (list.size() >= MAX) list.erase(list.begin());
    const BlockInfo& info = blockInfo(block);
    // A random 4x4 texel window of the block's side texture.
    uint32_t u = (uint32_t)rnd(0, 12.99f), v = (uint32_t)rnd(0, 12.99f);
    uint32_t packed = (uint32_t)info.tex[0] << 3 | (uint32_t)isEmissive(block) << 15 | (uint32_t)KindCutout << 16 | u << 18 | v << 23;

    // Rough lighting: open sky above, or the glow of a light block itself.
    int x = (int)std::floor(pos.x), y = (int)std::floor(pos.y), z = (int)std::floor(pos.z);
    uint32_t sky = world.topBlock(x, z) <= y ? 255 : 100;
    uint32_t light = sky | (uint32_t)(info.emit[0] * 17) << 8 | (uint32_t)(info.emit[1] * 17) << 16 | (uint32_t)(info.emit[2] * 17) << 24;
    list.push_back({pos, vel, rnd(0.4f, 1.2f), rnd(0.05f, 0.09f), packed, light});
}

void Particles::burst(const World& world, glm::ivec3 p, BlockId block) {
    if (block == Air || isLiquid(block)) return;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) {
                glm::vec3 local((i + rnd(0.2f, 0.8f)) / 4, (j + rnd(0.2f, 0.8f)) / 4, (k + rnd(0.2f, 0.8f)) / 4);
                glm::vec3 vel = (local - 0.5f) * 3.0f + glm::vec3(rnd(-0.6f, 0.6f), rnd(1.0f, 2.5f), rnd(-0.6f, 0.6f));
                spawn(world, glm::dvec3(p) + glm::dvec3(local), vel, block);
            }
}

void Particles::chip(const World& world, const RayHit& hit, BlockId block) {
    if (block == Air || isLiquid(block)) return;
    glm::dvec3 n(hit.normal);
    glm::dvec3 jitter(rnd(-0.4f, 0.4f), rnd(-0.4f, 0.4f), rnd(-0.4f, 0.4f));
    glm::dvec3 pos = hit.point + n * 0.08 + jitter * (glm::dvec3(1) - glm::abs(n));
    glm::vec3 vel = glm::vec3(n) * rnd(0.8f, 1.6f) + glm::vec3(rnd(-0.5f, 0.5f), rnd(0.5f, 1.5f), rnd(-0.5f, 0.5f));
    spawn(world, pos, vel, block);
}

void Particles::update(const World& world, double dt) {
    float fdt = (float)dt;
    for (Particle& p : list) {
        p.life -= fdt;
        p.vel.y -= GRAVITY * fdt;
        p.vel *= std::exp(-DRAG * fdt);
        // Move one axis at a time and stop on whatever it runs into.
        for (int a = 0; a < 3; ++a) {
            glm::dvec3 next = p.pos;
            next[a] += p.vel[a] * dt;
            if (solidAt(world, next)) {
                if (a == 1 && p.vel.y < 0) p.vel.x *= 0.6f, p.vel.z *= 0.6f;
                p.vel[a] = 0;
            } else {
                p.pos = next;
            }
        }
    }
    list.erase(std::remove_if(list.begin(), list.end(), [](const Particle& p) { return p.life <= 0; }), list.end());
}

const std::vector<Vertex>& Particles::mesh(glm::dvec3 cam, float yaw, float pitch) {
    verts.clear();
    glm::vec3 fwd(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
    glm::vec3 right(std::cos(yaw), 0, std::sin(yaw)), up = glm::cross(right, fwd);
    // The axis normal closest to facing the camera, for the lighting.
    glm::vec3 a = glm::abs(fwd);
    uint32_t normal = a.x > a.y && a.x > a.z ? (fwd.x > 0 ? 1 : 0) : a.y > a.z ? (fwd.y > 0 ? 3 : 2) : (fwd.z > 0 ? 5 : 4);
    for (const Particle& p : list) {
        glm::vec3 c(p.pos - cam);
        if (glm::dot(c, fwd) < 0.1f) continue;
        glm::vec3 r = right * p.size, u = up * p.size;
        const glm::vec3 corners[4] = {c - r - u, c + r - u, c + r + u, c - r + u};
        const uint32_t uv[4] = {0u << 18 | 4u << 23, 4u << 18 | 4u << 23, 4u << 18, 0u};  // offsets added to the window
        for (int i = 0; i < 4; ++i) {
            uint32_t packed = (p.packed + uv[i]) | normal | 3u << 13;
            verts.push_back({corners[i].x, corners[i].y, corners[i].z, packed, p.light});
        }
    }
    return verts;
}

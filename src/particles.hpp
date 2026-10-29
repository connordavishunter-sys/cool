#pragma once
#include "player.hpp"
#include "world.hpp"

#include <glm/glm.hpp>

#include <random>
#include <vector>

// Little textured chips that fly off blocks being mined or broken. They fall, bounce off the terrain
// and are drawn as camera-facing cutout quads through the world pipeline, so every lighting mode lights them.
class Particles {
public:
    void burst(const World& world, glm::ivec3 p, uint8_t block);  // a block was broken
    void chip(const World& world, const RayHit& hit, uint8_t block);  // a block is being mined
    void update(const World& world, double dt);
    void clear() { list.clear(); }
    // Quads in render space (relative to the camera), in the world vertex format.
    const std::vector<Vertex>& mesh(glm::dvec3 cam, float yaw, float pitch);

private:
    struct Particle {
        glm::dvec3 pos;
        glm::vec3 vel;
        float life, size;
        uint32_t packed, light;  // texture layer, uv window and emissive bit; light as the mesher packs it
    };
    static constexpr size_t MAX = 2048;
    std::vector<Particle> list;
    std::vector<Vertex> verts;
    std::mt19937 rng{1234};

    float rnd(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
    void spawn(const World& world, glm::dvec3 pos, glm::vec3 vel, uint8_t block);
};

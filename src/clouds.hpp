#pragma once
#include "noise.hpp"
#include "world.hpp"

#include <glm/glm.hpp>

#include <vector>

// A layer of blocky clouds drifting east, drawn as translucent boxes through the world pipeline.
class Clouds {
public:
    static constexpr double HEIGHT = 192, THICKNESS = 4, CELL = 12, SPEED = 0.8;  // blocks, blocks per second

    explicit Clouds(uint64_t seed) : noise(seed ^ 0xC10D5C10D5ull) {}
    // Camera-relative quads in the world vertex format, out to radius blocks.
    const std::vector<Vertex>& mesh(glm::dvec3 cam, double seconds, double radius);

private:
    Noise noise;
    std::vector<Vertex> verts;
    bool cloudAt(int64_t cx, int64_t cz) const;
};

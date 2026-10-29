#include "clouds.hpp"

#include <cmath>
#include <cstring>

namespace {
// Face corners as fractions of the box, counter-clockwise seen from outside, in the mesher's face order.
const int CORNERS[6][4][3] = {
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}},
};
const uint32_t UV[4] = {16u << 23, 16u << 18 | 16u << 23, 16u << 18, 0};

uint32_t cloudLayer() {
    for (int i = 0; i < kBlockTextureCount; ++i)
        if (!std::strcmp(kBlockTextures[i], "cloud")) return (uint32_t)i;
    return 0;
}
}  // namespace

bool Clouds::cloudAt(int64_t cx, int64_t cz) const { return noise.fbm2(cx * 0.13f, cz * 0.13f, 2) > 0.12f; }

const std::vector<Vertex>& Clouds::mesh(glm::dvec3 cam, double seconds, double radius) {
    static const uint32_t layer = cloudLayer();
    verts.clear();
    const double drift = std::fmod(seconds * SPEED, CELL * 4096);
    const int64_t ccx = (int64_t)std::floor((cam.x - drift) / CELL), ccz = (int64_t)std::floor(cam.z / CELL);
    const int r = (int)std::ceil(radius / CELL);
    // Bit 12 marks clouds so the shader fogs them by horizontal distance; full AO, translucent kind, open sky.
    const uint32_t bits = layer << 3 | 1u << 12 | 3u << 13 | (uint32_t)KindTranslucent << 16;
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dz * dz > r * r) continue;
            const int64_t cx = ccx + dx, cz = ccz + dz;
            if (!cloudAt(cx, cz)) continue;
            const glm::dvec3 lo(cx * CELL + drift, HEIGHT, cz * CELL), size(CELL, THICKNESS, CELL), hi = lo + size;
            for (int f = 0; f < 6; ++f) {
                // Only faces looking at the camera, and no walls between neighbouring clouds.
                const int axis = f / 2;
                const bool pos = f % 2 == 0;
                if (pos ? cam[axis] <= hi[axis] : cam[axis] >= lo[axis]) continue;
                if (axis == 0 && cloudAt(cx + (pos ? 1 : -1), cz)) continue;
                if (axis == 2 && cloudAt(cx, cz + (pos ? 1 : -1))) continue;
                for (int c = 0; c < 4; ++c) {
                    glm::vec3 p(lo + glm::dvec3(CORNERS[f][c][0], CORNERS[f][c][1], CORNERS[f][c][2]) * size - cam);
                    verts.push_back({p.x, p.y, p.z, bits | (uint32_t)f | UV[c], 255});
                }
            }
        }
    return verts;
}

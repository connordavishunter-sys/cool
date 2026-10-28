#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>

inline uint64_t splitmix64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Ken Perlin's improved noise with a seeded permutation table.
class Noise {
public:
    explicit Noise(uint64_t seed) {
        std::array<uint8_t, 256> perm;
        std::iota(perm.begin(), perm.end(), 0);
        for (int i = 255; i > 0; --i) std::swap(perm[i], perm[splitmix64(seed) % (i + 1)]);
        for (int i = 0; i < 512; ++i) p[i] = perm[i & 255];
    }

    float noise(float x, float y, float z) const {
        int X = (int)std::floor(x) & 255, Y = (int)std::floor(y) & 255, Z = (int)std::floor(z) & 255;
        x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
        float u = fade(x), v = fade(y), w = fade(z);
        int A = p[X] + Y, AA = p[A] + Z, AB = p[A + 1] + Z;
        int B = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;
        return lerp(w, lerp(v, lerp(u, grad(p[AA], x, y, z), grad(p[BA], x - 1, y, z)),
                               lerp(u, grad(p[AB], x, y - 1, z), grad(p[BB], x - 1, y - 1, z))),
                       lerp(v, lerp(u, grad(p[AA + 1], x, y, z - 1), grad(p[BA + 1], x - 1, y, z - 1)),
                               lerp(u, grad(p[AB + 1], x, y - 1, z - 1), grad(p[BB + 1], x - 1, y - 1, z - 1))));
    }

    float fbm2(float x, float z, int octaves) const {
        float sum = 0, amp = 1, norm = 0;
        for (int i = 0; i < octaves; ++i) {
            sum += noise(x, 0.37f * i, z) * amp;
            norm += amp;
            amp *= 0.5f; x *= 2; z *= 2;
        }
        return sum / norm;
    }

private:
    std::array<uint8_t, 512> p;
    static float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
    static float lerp(float t, float a, float b) { return a + t * (b - a); }
    static float grad(int h, float x, float y, float z) {
        h &= 15;
        float u = h < 8 ? x : y, v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }
};

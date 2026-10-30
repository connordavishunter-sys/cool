#include "world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- terrain

Terrain::Terrain(uint64_t seed)
    : seed(seed), continent(seed), hills(seed + 1), ridges(seed + 2), cave1(seed + 3), cave2(seed + 4), forest(seed + 5) {}

static float smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

int Terrain::height(int x, int z) const {
    float c = continent.fbm2(x * 0.0015f, z * 0.0015f, 4) * 1.6f;
    float h = hills.fbm2(x * 0.009f, z * 0.009f, 5);
    float r = 1.0f - std::fabs(ridges.fbm2(x * 0.004f, z * 0.004f, 4) * 1.4f);
    float mountain = smooth(0.05f, 0.45f, c) * r * r * 70.0f;
    float y = 64.0f + c * 30.0f + h * (6.0f + std::max(c, 0.0f) * 14.0f) + mountain;
    return std::clamp((int)y, 4, CH - 8);
}

bool Terrain::cave(int x, int y, int z, int h) const {
    if (y <= 2 || !(y < h - 4 || (y <= h && h > SEA_LEVEL + 2))) return false;
    float n1 = cave1.noise(x * 0.045f, y * 0.075f, z * 0.045f);
    float n2 = cave2.noise(x * 0.045f, y * 0.075f, z * 0.045f);
    return n1 * n1 + n2 * n2 < 0.0035f;
}

static uint64_t hash3(uint64_t a, int64_t b, int64_t c) {
    uint64_t s = a ^ (uint64_t)b * 0x9E3779B97F4A7C15ull ^ (uint64_t)c * 0xC2B2AE3D27D4EB4Full;
    return splitmix64(s);
}

// Trees sit on a 5x5 cell grid with one candidate per cell, so canopies never merge into blobs.
// Every chunk re-derives the trees around it and keeps the blocks that land inside it.
void Terrain::placeTrees(Chunk& c) const {
    const int CELL = 5, R = 2;
    auto put = [&](int wx, int y, int wz, uint8_t b) {
        int x = wx - c.cx * CW, z = wz - c.cz * CW;
        if (x < 0 || z < 0 || x >= CW || z >= CW || y >= CH) return;
        uint8_t& cur = c.blocks[Chunk::index(x, y, z)];
        if (cur == Air || (b == Log && cur == Leaves)) cur = b;
    };
    auto floorDiv = [](int a, int b) { return (a - (((a % b) + b) % b)) / b; };
    int x0 = floorDiv(c.cx * CW - R, CELL), x1 = floorDiv(c.cx * CW + CW - 1 + R, CELL);
    int z0 = floorDiv(c.cz * CW - R, CELL), z1 = floorDiv(c.cz * CW + CW - 1 + R, CELL);
    for (int gz = z0; gz <= z1; ++gz)
        for (int gx = x0; gx <= x1; ++gx) {
            uint64_t r = hash3(seed, gx, gz);
            int tx = gx * CELL + (int)(r % 3), tz = gz * CELL + (int)((r >> 8) % 3);
            float density = smooth(-0.1f, 0.35f, forest.fbm2(tx * 0.006f, tz * 0.006f, 3));
            if ((r >> 16 & 0xFFFF) / 65536.0f >= 0.03f + density * 0.6f) continue;
            int h = height(tx, tz);
            if (h <= SEA_LEVEL + 1 || h > 115 || cave(tx, h, tz, h)) continue;

            int trunk = 4 + (int)((r >> 32) % 3), top = h + trunk;
            for (int y = top - 3; y <= top + 1; ++y) {
                int rad = y >= top ? 1 : 2;
                for (int dz = -rad; dz <= rad; ++dz)
                    for (int dx = -rad; dx <= rad; ++dx) {
                        bool corner = std::abs(dx) == rad && std::abs(dz) == rad;
                        // Trim corners: always on the top layer, randomly on the wide layers.
                        if (corner && (y == top + 1 || (hash3(r, dx + 8 * y, dz) & 1))) continue;
                        put(tx + dx, y, tz + dz, Leaves);
                    }
            }
            for (int y = h + 1; y <= top; ++y) put(tx, y, tz, Log);
            put(tx, h, tz, Dirt);
        }
}

void Terrain::generate(Chunk& c) const {
    int heights[CW * CW];
    for (int z = 0; z < CW; ++z)
        for (int x = 0; x < CW; ++x) {
            int wx = c.cx * CW + x, wz = c.cz * CW + z;
            int h = heights[z * CW + x] = height(wx, wz);
            bool beach = h <= SEA_LEVEL + 1;
            for (int y = 0; y <= std::max(h, SEA_LEVEL); ++y) {
                uint8_t b;
                if (y > h) b = Water;
                else if (y < h - 3) b = Stone;
                else if (y < h) b = beach ? Sand : Dirt;
                else b = beach ? Sand : Grass;

                if (b != Water && cave(wx, y, wz, h)) b = y <= LAVA_LEVEL ? Lava : Air;
                c.blocks[Chunk::index(x, y, z)] = b;
            }
        }
    placeTrees(c);

    // Flowers grow in single-colour patches on an 8x8 cell grid.
    for (int z = 0; z < CW; ++z)
        for (int x = 0; x < CW; ++x) {
            int wx = c.cx * CW + x, wz = c.cz * CW + z, h = heights[z * CW + x];
            uint64_t patch = hash3(seed ^ 0xF1F1F1, wx >> 3, wz >> 3);
            if (patch % 4 || hash3(seed, wx, wz) % 6 || h + 1 >= CH) continue;
            if (c.get(x, h, z) != Grass || c.get(x, h + 1, z) != Air) continue;
            c.blocks[Chunk::index(x, h + 1, z)] = (uint8_t)(RedFlower + (patch >> 8) % 6);
        }
}

// ---------------------------------------------------------------- thread pool

ThreadPool::ThreadPool(int n) {
    for (int i = 0; i < n; ++i)
        threads.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !high.empty() || !low.empty(); });
                    if (stopping) return;
                    auto& q = high.empty() ? low : high;
                    job = std::move(q.front());
                    q.pop_front();
                }
                job();
            }
        });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    cv.notify_all();
    for (auto& t : threads) t.join();
}

void ThreadPool::push(std::function<void()> job, bool urgent) {
    {
        std::lock_guard lock(mutex);
        (urgent ? high : low).push_back(std::move(job));
    }
    cv.notify_one();
}

// ---------------------------------------------------------------- meshing + lighting

namespace {

constexpr int R = 48;  // light region: the chunk plus one neighbour chunk on each side
inline int ri(int x, int y, int z) { return (y * R + z) * R + x; }

const int DIRS[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
// Corners in BL, BR, TR, TL order as seen from outside (counter-clockwise).
const int CORNERS[6][4][3] = {
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}},
};

inline bool occludes(uint8_t b) { return blockInfo(b).opaque || isPartial(b); }

struct Scratch {
    std::vector<uint8_t> blk, sky, lr, lg, lb;
    std::vector<uint32_t> queue;
    std::vector<uint64_t> emitters;  // emission colour << 32 | cell
    std::vector<float> acc;          // summed linear block light, RGB per cell
};

// Light levels are perceptual: each level down is about 15% less light, so two equal lights add about 4 levels.
constexpr float LEVEL_STEP = 0.85f;
const auto LEVEL_LIN = [] {
    std::array<float, 16> t{};
    for (int l = 1; l < 16; ++l) t[l] = std::pow(LEVEL_STEP, 15 - l);
    return t;
}();
uint8_t linLevel(float v) {
    return v <= 0 ? 0 : (uint8_t)std::clamp((int)std::lround(15 - std::log(v) / std::log(LEVEL_STEP)), 0, 15);
}
thread_local Scratch S;

struct Mesher {
    int top = 0;  // everything above is open air with full skylight

    uint8_t block(int x, int y, int z) const { return y < 0 || y >= CH ? (uint8_t)Air : S.blk[ri(x, y, z)]; }
    uint8_t cells(uint8_t b, int x, int y, int z) const {  // occupied half-block cells, with stair corners
        return stairMask(b, [&](int px, int py, int pz) { return block(px, py, pz); }, x, y, z);
    }
    bool opaqueAt(int x, int y, int z) const {  // stops light and casts AO
        if (y < 0) return true;
        return y < CH && occludes(S.blk[ri(x, y, z)]);
    }
    glm::ivec4 light(int x, int y, int z) const {
        if (y < 0) return {0, 0, 0, 0};
        if (y > top) return {15, 0, 0, 0};
        int i = ri(x, y, z);
        return {S.sky[i], S.lr[i], S.lg[i], S.lb[i]};
    }

    void computeLight() {
        const int n = R * R * (top + 1);
        S.sky.assign(n, 0); S.lr.assign(n, 0); S.lg.assign(n, 0); S.lb.assign(n, 0);
        auto& q = S.queue;

        // Skylight falls straight down, then spreads sideways.
        for (int z = 0; z < R; ++z)
            for (int x = 0; x < R; ++x) {
                int v = 15;
                for (int y = top; y >= 0; --y) {
                    uint8_t b = S.blk[ri(x, y, z)];
                    if (occludes(b) || b == Lava) v = 0;
                    else if (b == Water || b == Leaves) v = std::max(0, v - (b == Water ? 2 : 1));
                    S.sky[ri(x, y, z)] = (uint8_t)v;
                    if (v == 0) { for (--y; y >= 0; --y) S.sky[ri(x, y, z)] = 0; }
                }
            }
        q.clear();
        for (int y = 0; y <= top; ++y)
            for (int z = 0; z < R; ++z)
                for (int x = 0; x < R; ++x) {
                    int v = S.sky[ri(x, y, z)];
                    if (v < 2) continue;
                    for (int d : {0, 1, 4, 5}) {
                        int nx = x + DIRS[d][0], nz = z + DIRS[d][2];
                        if (nx < 0 || nz < 0 || nx >= R || nz >= R) continue;
                        int ni = ri(nx, y, nz);
                        if (S.sky[ni] < v - 1 && !occludes(S.blk[ni])) { q.push_back(ri(x, y, z)); break; }
                    }
                }
        flood(S.sky, nullptr, nullptr);

        // Each light colour floods on its own and the colours are then added in linear light, so where lights
        // overlap their colours mix (and brighten) instead of the brightest one winning.
        auto& em = S.emitters;
        em.clear();
        for (int i = 0; i < n; ++i) {
            const auto& e = blockInfo(S.blk[i]).emit;
            if (e[0] | e[1] | e[2]) em.push_back((uint64_t)(e[0] << 8 | e[1] << 4 | e[2]) << 32 | (uint32_t)i);
        }
        std::sort(em.begin(), em.end());
        S.acc.assign(3 * n, 0.0f);
        for (size_t run = 0; run < em.size();) {
            const uint64_t colour = em[run] >> 32;
            const auto& e = blockInfo(S.blk[(uint32_t)em[run]]).emit;
            q.clear();
            for (; run < em.size() && em[run] >> 32 == colour; ++run) {
                int i = (uint32_t)em[run];
                S.lr[i] = e[0]; S.lg[i] = e[1]; S.lb[i] = e[2];
                q.push_back(i);
            }
            flood(S.lr, &S.lg, &S.lb);
            for (int i : q) {  // cells can be queued twice; zeroing them makes the repeat a no-op
                if (!(S.lr[i] | S.lg[i] | S.lb[i])) continue;
                S.acc[3 * i] += LEVEL_LIN[S.lr[i]]; S.acc[3 * i + 1] += LEVEL_LIN[S.lg[i]]; S.acc[3 * i + 2] += LEVEL_LIN[S.lb[i]];
                S.lr[i] = S.lg[i] = S.lb[i] = 0;
            }
        }
        if (!em.empty())
            for (int i = 0; i < n; ++i) {
                S.lr[i] = linLevel(S.acc[3 * i]); S.lg[i] = linLevel(S.acc[3 * i + 1]); S.lb[i] = linLevel(S.acc[3 * i + 2]);
            }

        // Slabs and stairs stop light, but take their brightest neighbour's so faces looking into them aren't black.
        for (int y = 0; y <= top; ++y)
            for (int z = 1; z < R - 1; ++z)
                for (int x = 1; x < R - 1; ++x) {
                    int i = ri(x, y, z);
                    if (!isPartial(S.blk[i])) continue;
                    glm::ivec4 l = light(x, y, z);
                    for (const auto& d : DIRS)
                        if (y + d[1] >= 0) l = glm::max(l, light(x + d[0], y + d[1], z + d[2]));
                    S.sky[i] = (uint8_t)l.x; S.lr[i] = (uint8_t)l.y; S.lg[i] = (uint8_t)l.z; S.lb[i] = (uint8_t)l.w;
                }
    }

    // Breadth-first light propagation over one (sky) or three (RGB) channels.
    void flood(std::vector<uint8_t>& a, std::vector<uint8_t>* b, std::vector<uint8_t>* c) {
        auto& q = S.queue;
        for (size_t head = 0; head < q.size(); ++head) {
            int i = q[head];
            int x = i % R, z = (i / R) % R, y = i / (R * R);
            for (const auto& d : DIRS) {
                int nx = x + d[0], ny = y + d[1], nz = z + d[2];
                if (nx < 0 || nz < 0 || nx >= R || nz >= R || ny < 0 || ny > top) continue;
                int ni = ri(nx, ny, nz);
                uint8_t nb = S.blk[ni];
                if (occludes(nb)) continue;
                int att = nb == Water || nb == Leaves ? 2 : 1;
                bool grew = false;
                auto spread = [&](std::vector<uint8_t>& ch, int loss) {
                    int v = ch[i] - att - loss;
                    if (v > ch[ni]) { ch[ni] = (uint8_t)v; grew = true; }
                };
                if (b) {
                    const uint8_t* t = tintLoss(nb);
                    spread(a, t[0]); spread(*b, t[1]); spread(*c, t[2]);
                } else {
                    spread(a, 0);
                }
                if (grew) q.push_back(ni);
            }
        }
    }

    std::vector<Vertex>& listFor(MeshData& m, BlockKind k, std::vector<Vertex>* lists) {
        return k == KindOpaque ? m.verts : lists[k - 1];
    }

    MeshData mesh() {
        MeshData m;
        std::vector<Vertex> lists[3];  // cutout, water, translucent
        m.minY = 1e9f; m.maxY = -1e9f;

        for (int y = 0; y <= top; ++y)
            for (int z = 16; z < 32; ++z)
                for (int x = 16; x < 32; ++x) {
                    uint8_t b = S.blk[ri(x, y, z)];
                    if (b == Air) continue;
                    const BlockInfo& info = blockInfo(b);
                    auto& out = listFor(m, info.kind, lists);
                    bool emissive = isEmissive(b);
                    bool exposed = info.model == ModelCross ? (emitCross(out, x, y, z, b), true)
                                 : info.model == ModelPane  ? emitPane(out, x, y, z, b)
                                 : isPanel(b)               ? emitPanel(out, x, y, z, b)
                                                            : emitCube(out, x, y, z, b, emissive);
                    if (!exposed) continue;
                    m.minY = std::min(m.minY, (float)y);
                    m.maxY = std::max(m.maxY, (float)y + 1);
                    if (emissive) m.lights.push_back({x - 16, y, z - 16, b});  // buried lava would flood the light list
                }

        m.solidQuads = (uint32_t)m.verts.size() / 4;
        m.cutoutQuads = (uint32_t)lists[0].size() / 4;
        m.waterQuads = (uint32_t)lists[1].size() / 4;
        m.translucentQuads = (uint32_t)lists[2].size() / 4;
        for (auto& l : lists) m.verts.insert(m.verts.end(), l.begin(), l.end());
        if (m.verts.empty()) m.minY = m.maxY = 0;
        return m;
    }

    // Cubes, including slabs and stairs: each face is split into quarters wherever a half-block cell is
    // exposed, either on the block's boundary or on its middle plane (the tread of a stair).
    bool emitCube(std::vector<Vertex>& out, int x, int y, int z, uint8_t b, bool emissive) {
        bool liquidTop = isLiquid(b) && block(x, y + 1, z) != b, any = false;
        for (int f = 0; f < 6; ++f) {
            const int* d = DIRS[f];
            if (y + d[1] < 0) continue;
            uint8_t nb = block(x + d[0], y + d[1], z + d[2]);
            const BlockInfo& ni = blockInfo(nb);
            bool covers = ni.opaque || (nb == b && b != Leaves);  // leaves keep inner faces
            const uint8_t mask = cells(b, x, y, z), nmask = isPartial(nb) ? cells(nb, x + d[0], y + d[1], z + d[2]) : 0;
            if (mask == 0xFF && covers) continue;

            const int axis = f / 2, ta = (axis + 1) % 3, tb = (axis + 2) % 3, side = f % 2 == 0;
            uint8_t outer = 0, inner = 0;
            for (int q = 0; q < 4; ++q) {
                int c[3];
                c[axis] = side; c[ta] = q & 1; c[tb] = q >> 1;
                auto bit = [&](uint8_t mask) { return mask >> (c[0] + 2 * c[2] + 4 * c[1]) & 1; };
                bool filled = bit(mask);
                c[axis] = 1 - side;  // the neighbour's touching cell, or this block's inner cell
                if (filled) outer |= (isPartial(nb) ? !bit(nmask) : !covers) << q;
                else inner |= bit(mask) << q;
            }
            for (int layer = 0; layer < 2; ++layer) {
                uint8_t set = layer ? inner : outer;
                float lo[3], hi[3];
                lo[axis] = hi[axis] = layer ? 0.5f : (float)side;
                for (int q = 0; q < 4; ++q) {
                    if (set == 15) { lo[ta] = lo[tb] = 0; hi[ta] = hi[tb] = 1; q = 4; }
                    else if (set >> q & 1) { lo[ta] = (q & 1) * 0.5f; lo[tb] = (q >> 1) * 0.5f; hi[ta] = lo[ta] + 0.5f; hi[tb] = lo[tb] + 0.5f; }
                    else continue;
                    emitQuad(out, x, y, z, f, lo, hi, b, emissive, liquidTop, false);
                    any = true;
                }
            }
        }
        return any;
    }

    // A panel is one box; its faces on the cell boundary are hidden where the neighbour covers them.
    bool emitPanel(std::vector<Vertex>& out, int x, int y, int z, uint8_t b) {
        Box boxes[8];
        blockBoxes(b, 0, boxes);
        const Box& bx = boxes[0];
        bool any = false;
        for (int f = 0; f < 6; ++f) {
            const int* d = DIRS[f];
            const int axis = f / 2;
            const bool side = f % 2 == 0;
            if ((side ? bx.hi[axis] == 1 : bx.lo[axis] == 0) && (y + d[1] < 0 || covered(bx, f, b, x + d[0], y + d[1], z + d[2]))) continue;
            emitQuad(out, x, y, z, f, bx.lo, bx.hi, b, false, false, false);
            any = true;
        }
        return any;
    }
    bool covered(const Box& bx, int f, uint8_t b, int nx, int ny, int nz) const {
        const uint8_t nb = block(nx, ny, nz);
        const int axis = f / 2;
        if (blockInfo(nb).opaque) return true;
        if (isPanel(nb)) return axis == 1 ? panelTop(nb) == (f == 3) : panelTop(nb) == panelTop(b) && panelEighths(nb) >= panelEighths(b);
        if (!isPartial(nb)) return false;
        const uint8_t m = cells(nb, nx, ny, nz);
        for (int i = 0; i < 8; ++i) {  // every neighbour cell touching the face must be filled
            const int c[3] = {i & 1, i >> 2, i >> 1 & 1};
            bool touches = c[axis] == (f % 2);
            for (int a = 0; a < 3; ++a)
                if (a != axis) touches &= c[a] * 0.5f < bx.hi[a] && c[a] * 0.5f + 0.5f > bx.lo[a];
            if (touches && !(m >> i & 1)) return false;
        }
        return true;
    }

    bool emitPane(std::vector<Vertex>& out, int x, int y, int z, uint8_t b) {
        Box boxes[8];
        int n = blockBoxes(b, blockShape(b, [&](int px, int py, int pz) { return block(px, py, pz); }, x, y, z), boxes);
        bool any = false;
        for (int i = 0; i < n; ++i)
            for (int f = 0; f < 6; ++f) {
                const Box& bx = boxes[i];
                const int* d = DIRS[f];
                int axis = f / 2;
                bool side = f % 2 == 0;
                if (bx.skip >> f & 1) continue;
                if ((side ? bx.hi[axis] == 1 : bx.lo[axis] == 0) && (y + d[1] < 0 || paneConnects(block(x + d[0], y + d[1], z + d[2])))) continue;
                emitQuad(out, x, y, z, f, bx.lo, bx.hi, b, false, false, true);
                any = true;
            }
        return any;
    }

    // Two crossed double-sided quads, lit by the flower's own cell.
    void emitCross(std::vector<Vertex>& out, int x, int y, int z, uint8_t b) {
        const float a = 0.5f - 0.3536f, c = 0.5f + 0.3536f;  // planes one block wide
        const float ends[2][4] = {{a, a, c, c}, {a, c, c, a}};
        const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        glm::ivec4 l = light(x, y, z) * 17, ls[4] = {l, l, l, l};
        const int ao[4] = {3, 3, 3, 3};
        for (const auto& e : ends)
            for (int back = 0; back < 2; ++back) {
                float x0 = e[back * 2], z0 = e[back * 2 + 1], x1 = e[2 - back * 2], z1 = e[3 - back * 2];
                glm::vec3 p[4] = {{x0, 0, z0}, {x1, 0, z1}, {x1, 1, z1}, {x0, 1, z0}};
                for (auto& v : p) v += glm::vec3(x - 16, y, z - 16);
                pushQuad(out, p, uv, ls, ao, 2u | (uint32_t)blockInfo(b).tex[2] << 3 | (uint32_t)KindCutout << 16);
            }
    }

    // Emits the face f of box [lo, hi] (block-local units). Light and AO are computed for the whole face of
    // the cell it looks into (or its own cell for panes) and interpolated across the box.
    void emitQuad(std::vector<Vertex>& out, int x, int y, int z, int f, const float* lo, const float* hi, uint8_t b,
                  bool emissive, bool liquidTop, bool pane) {
        const BlockInfo& info = blockInfo(b);
        const int fx = x + (pane ? 0 : DIRS[f][0]), fy = y + (pane ? 0 : DIRS[f][1]), fz = z + (pane ? 0 : DIRS[f][2]);
        const int axis = f / 2, ta = (axis + 1) % 3, tb = (axis + 2) % 3;
        const glm::vec2 cornerUV[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

        glm::vec4 cl[4];
        float cao[4];
        for (int c = 0; c < 4; ++c) {
            const int* p = CORNERS[f][c];
            int da[3] = {0, 0, 0}, db[3] = {0, 0, 0};
            da[ta] = p[ta] ? 1 : -1;
            db[tb] = p[tb] ? 1 : -1;
            bool s1 = opaqueAt(fx + da[0], fy + da[1], fz + da[2]);
            bool s2 = opaqueAt(fx + db[0], fy + db[1], fz + db[2]);
            bool cc = opaqueAt(fx + da[0] + db[0], fy + da[1] + db[1], fz + da[2] + db[2]);
            cao[c] = info.kind == KindWater || pane ? 3 : (s1 && s2) ? 0 : 3 - (s1 + s2 + cc);

            // Smooth lighting: average the open cells touching this corner.
            glm::ivec4 sum = light(fx, fy, fz);
            int count = 1;
            if (!s1) { sum += light(fx + da[0], fy + da[1], fz + da[2]); ++count; }
            if (!s2) { sum += light(fx + db[0], fy + db[1], fz + db[2]); ++count; }
            if (!cc && (!s1 || !s2)) { sum += light(fx + da[0] + db[0], fy + da[1] + db[1], fz + da[2] + db[2]); ++count; }
            cl[c] = glm::vec4(sum * 17) / (float)count;
        }

        glm::vec3 pos[4];
        glm::vec2 uv[4];
        glm::ivec4 l[4];
        int ao[4];
        uint32_t wave = 0;  // corners on a liquid's surface, which the vertex shader animates
        for (int c = 0; c < 4; ++c) {
            const int* p = CORNERS[f][c];
            for (int i = 0; i < 3; ++i) pos[c][i] = p[i] ? hi[i] : lo[i];
            // Pane edges are thinner than the texture, so they show its one-pixel frame instead.
            float tu = pos[c][ta], tv = pos[c][tb];
            if (pane && hi[ta] - lo[ta] < 0.2f) tu = p[ta] / 16.0f;
            if (pane && hi[tb] - lo[tb] < 0.2f) tv = p[tb] / 16.0f;
            glm::vec4 lsum(0);
            float asum = 0;
            uv[c] = glm::vec2(0);
            for (int k = 0; k < 4; ++k) {
                const int* q = CORNERS[f][k];
                float w = (q[ta] ? pos[c][ta] : 1 - pos[c][ta]) * (q[tb] ? pos[c][tb] : 1 - pos[c][tb]);
                lsum += cl[k] * w;
                asum += cao[k] * w;
                uv[c] += cornerUV[k] * ((q[ta] ? tu : 1 - tu) * (q[tb] ? tv : 1 - tv));
            }
            l[c] = glm::ivec4(lsum + 0.5f);
            ao[c] = (int)(asum + 0.5f);
            if (liquidTop && pos[c][1] == 1) {
                pos[c][1] -= 0.125f;
                wave |= 1u << c;
            }
            pos[c] += glm::vec3(x - 16, y, z - 16);
        }
        pushQuad(out, pos, uv, l, ao, (uint32_t)f | (uint32_t)info.tex[f] << 3 | (uint32_t)emissive << 15 | (uint32_t)info.kind << 16, wave);
    }

    static void pushQuad(std::vector<Vertex>& out, const glm::vec3* pos, const glm::vec2* uv, const glm::ivec4* l, const int* ao, uint32_t bits,
                         uint32_t wave = 0) {
        Vertex v[4];
        for (int c = 0; c < 4; ++c) {
            v[c].x = pos[c].x; v[c].y = pos[c].y; v[c].z = pos[c].z;
            v[c].packed = bits | (wave >> c & 1u) << 11 | (uint32_t)ao[c] << 13 | (uint32_t)std::lround(uv[c].x * 16) << 18 | (uint32_t)std::lround(uv[c].y * 16) << 23;
            v[c].light = (uint32_t)l[c].x | (uint32_t)l[c].y << 8 | (uint32_t)l[c].z << 16 | (uint32_t)l[c].w << 24;
        }
        // Split along the brighter diagonal so AO interpolates symmetrically.
        int start = ao[0] + ao[2] < ao[1] + ao[3] ? 1 : 0;
        for (int i = 0; i < 4; ++i) out.push_back(v[(start + i) & 3]);
    }
};

}  // namespace

MeshData buildMesh(const std::array<std::shared_ptr<Chunk>, 9>& nb, std::shared_mutex& mutex) {
    S.blk.resize(R * R * CH);
    {
        std::shared_lock lock(mutex);
        for (int i = 0; i < 9; ++i) {
            int ox = (i % 3) * CW, oz = (i / 3) * CW;
            const auto& src = nb[i]->blocks;
            for (int y = 0; y < CH; ++y)
                for (int z = 0; z < CW; ++z)
                    std::memcpy(&S.blk[ri(ox, y, oz + z)], &src[Chunk::index(0, y, z)], CW);
        }
    }
    Mesher m;
    m.top = 0;
    for (int y = CH - 1; y >= 0 && !m.top; --y) {
        const uint8_t* row = &S.blk[ri(0, y, 0)];
        if (std::any_of(row, row + R * R, [](uint8_t b) { return b != Air; })) m.top = std::min(y + 1, CH - 1);
    }
    m.computeLight();
    MeshData data = m.mesh();
    data.cx = nb[4]->cx;
    data.cz = nb[4]->cz;
    return data;
}

// ---------------------------------------------------------------- world

World::World(fs::path dir_, uint64_t seed)
    : dir(std::move(dir_)), terrain(seed), pool(std::max(2, (int)std::thread::hardware_concurrency() - 2)) {
    fs::create_directories(dir / "chunks");
}

World::~World() { saveChunks(); }

Chunk* World::find(int cx, int cz) const {
    auto it = chunks.find(chunkKey(cx, cz));
    return it == chunks.end() ? nullptr : it->second.get();
}

uint8_t World::get(int x, int y, int z) const {
    if (y < 0 || y >= CH) return Air;
    int cx = floorDiv(x, CW), cz = floorDiv(z, CW);
    Chunk* c = find(cx, cz);
    return c ? c->get(x - cx * CW, y, z - cz * CW) : (uint8_t)Air;
}

bool World::isLoaded(int x, int z) const { return find(floorDiv(x, CW), floorDiv(z, CW)) != nullptr; }
bool World::isMeshed(int cx, int cz) const { return meshedOnce.count(chunkKey(cx, cz)) != 0; }

int World::topBlock(int x, int z) const {
    for (int y = CH - 1; y >= 0; --y)
        if (get(x, y, z) != Air) return y;
    return -1;
}

void World::set(int x, int y, int z, uint8_t b) {
    if (y < 0 || y >= CH) return;
    int cx = floorDiv(x, CW), cz = floorDiv(z, CW);
    Chunk* c = find(cx, cz);
    if (!c) return;
    {
        std::unique_lock lock(blockMutex);
        c->blocks[Chunk::index(x - cx * CW, y, z - cz * CW)] = b;
    }
    c->needsSave = true;
    // Light reaches up to 15 blocks, so every neighbour may change.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (Chunk* n = find(cx + dx, cz + dz)) n->meshDirty = n->urgent = true;
}

void World::update(int pcx, int pcz, int renderDist) {
    const int genRadius = renderDist + 2;
    if (offsetsRadius != genRadius) {
        offsets.clear();
        for (int z = -genRadius; z <= genRadius; ++z)
            for (int x = -genRadius; x <= genRadius; ++x)
                if (x * x + z * z <= genRadius * genRadius) offsets.push_back({x, z});
        std::sort(offsets.begin(), offsets.end(), [](auto a, auto b) { return a.x * a.x + a.y * a.y < b.x * b.x + b.y * b.y; });
        offsetsRadius = genRadius;
    }

    std::vector<std::shared_ptr<Chunk>> gens;
    std::vector<MeshData> meshes;
    {
        std::lock_guard lock(resultMutex);
        gens.swap(genResults);
        meshes.swap(meshResults);
    }
    for (auto& c : gens) {
        int64_t key = chunkKey(c->cx, c->cz);
        pendingGen.erase(key);
        chunks[key] = c;
    }
    for (auto& m : meshes) {
        --meshesInFlight;
        Chunk* c = find(m.cx, m.cz);
        if (!c) continue;
        c->meshing = false;
        meshedOnce.insert(chunkKey(m.cx, m.cz));
        readyMeshes.push_back(std::move(m));
    }

    // Unload far chunks.
    for (auto it = chunks.begin(); it != chunks.end();) {
        Chunk& c = *it->second;
        if (std::max(std::abs(c.cx - pcx), std::abs(c.cz - pcz)) > genRadius + 1) {
            if (c.needsSave) saveChunk(c);
            meshedOnce.erase(it->first);
            it = chunks.erase(it);
        } else {
            ++it;
        }
    }

    // Request generation, nearest first.
    const size_t maxPending = 48;
    for (auto o : offsets) {
        if (pendingGen.size() >= maxPending) break;
        int cx = pcx + o.x, cz = pcz + o.y;
        int64_t key = chunkKey(cx, cz);
        if (chunks.count(key) || pendingGen.count(key)) continue;
        pendingGen.insert(key);
        pool.push([this, cx, cz] {
            auto c = std::make_shared<Chunk>();
            c->cx = cx; c->cz = cz;
            if (!loadChunk(*c)) terrain.generate(*c);
            std::lock_guard lock(resultMutex);
            genResults.push_back(std::move(c));
        }, false);
    }

    // Request meshes for chunks whose neighbours are all present.
    for (auto o : offsets) {
        if (o.x * o.x + o.y * o.y > renderDist * renderDist) break;
        Chunk* c = find(pcx + o.x, pcz + o.y);
        if (!c || !c->meshDirty || c->meshing) continue;
        if (!c->urgent && meshesInFlight >= 32) continue;
        std::array<std::shared_ptr<Chunk>, 9> nb;
        bool complete = true;
        for (int i = 0; i < 9 && complete; ++i) {
            auto it = chunks.find(chunkKey(c->cx + i % 3 - 1, c->cz + i / 3 - 1));
            if (it == chunks.end()) complete = false;
            else nb[i] = it->second;
        }
        if (!complete) continue;
        c->meshing = true;
        c->meshDirty = false;
        bool urgent = c->urgent;
        c->urgent = false;
        ++meshesInFlight;
        pool.push([this, nb] {
            MeshData m = buildMesh(nb, blockMutex);
            std::lock_guard lock(resultMutex);
            meshResults.push_back(std::move(m));
        }, urgent);
    }
}

// ---------------------------------------------------------------- persistence

fs::path World::chunkPath(int cx, int cz) const {
    return dir / "chunks" / ("c." + std::to_string(cx) + "." + std::to_string(cz) + ".bin");
}

// Run-length encoded block data: (uint16 count, uint8 block) pairs.
void World::saveChunk(const Chunk& c) const {
    std::vector<uint8_t> out;
    for (int i = 0; i < CVOL;) {
        int j = i;
        while (j < CVOL && j - i < 65535 && c.blocks[j] == c.blocks[i]) ++j;
        int n = j - i;
        out.push_back(n & 255); out.push_back(n >> 8); out.push_back(c.blocks[i]);
        i = j;
    }
    std::ofstream f(chunkPath(c.cx, c.cz), std::ios::binary);
    f.write("ORD1", 4);
    f.write((const char*)out.data(), out.size());
}

bool World::loadChunk(Chunk& c) const {
    std::ifstream f(chunkPath(c.cx, c.cz), std::ios::binary);
    if (!f) return false;
    char magic[4];
    if (!f.read(magic, 4) || std::memcmp(magic, "ORD1", 4)) return false;
    int i = 0;
    uint8_t rec[3];
    while (i < CVOL && f.read((char*)rec, 3)) {
        int n = rec[0] | rec[1] << 8;
        n = std::min(n, CVOL - i);
        std::memset(&c.blocks[i], rec[2] < BlockCount ? rec[2] : (uint8_t)Air, n);
        i += n;
    }
    return i == CVOL;
}

void World::saveChunks() {
    for (auto& [key, c] : chunks)
        if (c->needsSave) {
            saveChunk(*c);
            c->needsSave = false;
        }
}

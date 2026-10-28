#pragma once
#include "blocks.hpp"
#include "noise.hpp"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

constexpr int CW = 16, CH = 256, CVOL = CW * CW * CH;
constexpr int SEA_LEVEL = 63;
constexpr int LAVA_LEVEL = 10;  // caves at or below this fill with lava

inline int64_t chunkKey(int cx, int cz) { return ((int64_t)cx << 32) | (uint32_t)cz; }
inline int floorDiv(int a, int b) { return (a >= 0 ? a : a - b + 1) / b; }

struct Chunk {
    int cx = 0, cz = 0;
    std::array<uint8_t, CVOL> blocks{};
    bool needsSave = false;   // edited since last written to disk
    bool meshDirty = true;
    bool urgent = false;      // remesh with priority (player edit)
    bool meshing = false;

    static int index(int x, int y, int z) { return (y * CW + z) * CW + x; }
    uint8_t get(int x, int y, int z) const { return blocks[index(x, y, z)]; }
};

struct Vertex {
    float x, y, z;
    uint32_t packed;  // normal:3 layer:8 unused:2 ao:2 emissive:1 kind:2 u:5 v:5 (uv in 16ths)
    uint32_t light;   // sky, r, g, b as unorm8
};

struct MeshData {
    int cx = 0, cz = 0;
    std::vector<Vertex> verts;  // quads: solid, cutout, water, then translucent
    uint32_t solidQuads = 0, cutoutQuads = 0, waterQuads = 0, translucentQuads = 0;
    float minY = 0, maxY = 0;
    std::vector<glm::ivec4> lights;  // emissive blocks: local x, y, local z, block id
};

class Terrain {
public:
    explicit Terrain(uint64_t seed);
    int height(int x, int z) const;
    void generate(Chunk& c) const;

private:
    uint64_t seed;
    Noise continent, hills, ridges, cave1, cave2, forest;
    bool cave(int x, int y, int z, int h) const;  // spaghetti caves: two noise fields both near zero
    void placeTrees(Chunk& c) const;
};

class ThreadPool {
public:
    explicit ThreadPool(int threads);
    ~ThreadPool();
    void push(std::function<void()> job, bool urgent);

private:
    std::vector<std::thread> threads;
    std::deque<std::function<void()>> high, low;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
};

class World {
public:
    World(std::filesystem::path dir, uint64_t seed);
    ~World();

    // Streams chunks around the given chunk coordinate; call once per frame.
    void update(int pcx, int pcz, int renderDist);

    uint8_t get(int x, int y, int z) const;
    void set(int x, int y, int z, uint8_t b);
    bool isLoaded(int x, int z) const;
    bool isMeshed(int cx, int cz) const;
    int topBlock(int x, int z) const;  // y of highest non-air block, -1 if none
    void saveChunks();

    std::vector<MeshData> takeMeshes() { return std::move(readyMeshes); }
    size_t loadedChunks() const { return chunks.size(); }
    const std::filesystem::path& directory() const { return dir; }

private:
    std::filesystem::path dir;
    Terrain terrain;
    std::unordered_map<int64_t, std::shared_ptr<Chunk>> chunks;
    std::unordered_set<int64_t> pendingGen, meshedOnce;
    std::shared_mutex blockMutex;  // guards Chunk::blocks against worker snapshots
    std::mutex resultMutex;
    std::vector<std::shared_ptr<Chunk>> genResults;
    std::vector<MeshData> meshResults, readyMeshes;
    std::vector<glm::ivec2> offsets;
    int offsetsRadius = -1;
    int meshesInFlight = 0;
    ThreadPool pool;

    Chunk* find(int cx, int cz) const;
    std::filesystem::path chunkPath(int cx, int cz) const;
    void saveChunk(const Chunk& c) const;
    bool loadChunk(Chunk& c) const;
};

MeshData buildMesh(const std::array<std::shared_ptr<Chunk>, 9>& neighbours, std::shared_mutex& mutex);

#pragma once
#include <cstdint>
#include <vector>

// Slabs, stairs, vertical slabs and sideways stairs come in every orientation for each of these materials.
constexpr int SHAPE_MATERIALS = 9, SHAPE_STATES = 18;
// Panels are 1/8 to 7/8 of a block thick, on the floor (states 0-6) or the ceiling (7-13) of their cell.
constexpr int PANEL_MATERIALS = 3, PANEL_STATES = 14;

enum Block : uint8_t {
    Air, Grass, Dirt, Stone, Sand, Planks, Log, Glass, Water,
    WhiteLight, RedLight, OrangeLight, YellowLight, BlueLight, PurpleLight,
    // New blocks are appended so ids in existing saves stay valid.
    Leaves, Lava, RedStainedGlass, OrangeStainedGlass, YellowStainedGlass, GreenStainedGlass, PurpleStainedGlass,
    RedFlower, OrangeFlower, YellowFlower, GreenFlower, BlueFlower, PurpleFlower,
    GlassPane, RedStainedPane, OrangeStainedPane, YellowStainedPane, GreenStainedPane, PurpleStainedPane,
    ShapedBase,  // ShapedBase + material * SHAPE_STATES + state
    PanelBase = ShapedBase + SHAPE_MATERIALS * SHAPE_STATES,  // PanelBase + material * PANEL_STATES + state
    BlockCount = PanelBase + PANEL_MATERIALS * PANEL_STATES
};

enum BlockKind : uint8_t { KindOpaque, KindCutout, KindWater, KindTranslucent };
enum BlockModel : uint8_t { ModelCube, ModelCross, ModelPane };

// Shaped block states: Slab 0-1 (bottom, top), VSlab 2-5 (half towards +X, -X, +Z, -Z),
// Stairs 6-13 (tall side towards +X, -X, +Z, -Z; +4 upside down), SideStairs 14-17 (missing quarter column x + 2z).
enum Shape { ShapeSlab, ShapeVSlab, ShapeStairs, ShapeSideStairs };

// Faces are ordered +X, -X, +Y, -Y, +Z, -Z everywhere.
struct BlockInfo {
    const char* name;
    uint8_t tex[6];   // texture array layer per face
    bool opaque;      // full cube that blocks light and hides neighbouring faces
    bool solid;       // collides with the player
    BlockKind kind;
    uint8_t emit[3];  // RGB block light emission, 0..15
    float hardness;   // seconds to break in survival; < 0 can't be broken
    BlockModel model = ModelCube;
    uint8_t mask = 0xFF;  // occupied half-block cells of a cube, bit x + 2z + 4y (for panels, the fully covered ones)
};

struct Box { float lo[3], hi[3]; uint8_t skip = 0; };  // block-local units; skip: faces hidden inside the block

const BlockInfo& blockInfo(uint8_t b);
inline bool isLiquid(uint8_t b) { return b == Water || b == Lava; }
inline bool isEmissive(uint8_t b) { const auto& e = blockInfo(b).emit; return e[0] | e[1] | e[2]; }
inline bool isFlower(uint8_t b) { return b >= RedFlower && b <= PurpleFlower; }
inline bool isPane(uint8_t b) { return b >= GlassPane && b <= PurpleStainedPane; }
inline bool isShaped(uint8_t b) { return b >= ShapedBase && b < PanelBase; }
inline bool isPanel(uint8_t b) { return b >= PanelBase && b < BlockCount; }
inline int panelEighths(uint8_t b) { return (b - PanelBase) % 7 + 1; }
inline bool panelTop(uint8_t b) { return (b - PanelBase) % PANEL_STATES >= 7; }
inline bool isPartial(uint8_t b) { return blockInfo(b).mask != 0xFF; }  // a shaped cube that doesn't fill its cell

uint8_t shapedId(uint8_t material, int state);
uint8_t shapeMaterial(uint8_t b);
uint8_t panelId(uint8_t material, int eighths, bool top);  // a full block at 8 eighths; Air if it has no panels
uint8_t panelMaterial(uint8_t b);
// Light levels each RGB channel loses passing through b: stained glass filters out the colours it doesn't pass.
const uint8_t* tintLoss(uint8_t b);
int shapeState(uint8_t b);
Shape shapeOf(int state);
uint8_t itemOf(uint8_t b);  // the inventory item a placed block counts as (shaped blocks lose their orientation)

constexpr uint8_t HALF_MASK[4] = {0xAA, 0x55, 0xCC, 0x33};  // the half of a cube towards +X, -X, +Z or -Z

// Stairs join their neighbours into corners, like the original game: a perpendicular stair behind one turns its
// step into an outer corner, one in front into an inner corner, unless that would break a straight run.
template <class Get> uint8_t stairMask(uint8_t b, Get get, int x, int y, int z) {
    const uint8_t mask = blockInfo(b).mask;
    if (!isShaped(b) || shapeOf(shapeState(b)) != ShapeStairs) return mask;
    const int s = shapeState(b), f = (s - 6) & 3;
    const bool upper = s >= 10;
    static constexpr int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    auto facingAt = [&](int dir) {  // facing of a matching stair next to this one, or -1
        uint8_t n = get(x + D[dir][0], y, z + D[dir][1]);
        if (!isShaped(n) || shapeOf(shapeState(n)) != ShapeStairs || (shapeState(n) >= 10) != upper) return -1;
        return (shapeState(n) - 6) & 3;
    };
    const uint8_t step = upper ? 0x0F : 0xF0;  // the layer holding the step
    int f2 = facingAt(f);
    if (f2 >= 0 && f2 >> 1 != f >> 1 && facingAt(f2 ^ 1) != f) return (mask & ~step) | (HALF_MASK[f] & HALF_MASK[f2] & step);
    f2 = facingAt(f ^ 1);
    if (f2 >= 0 && f2 >> 1 != f >> 1 && facingAt(f2) != f) return (mask & ~step) | ((HALF_MASK[f] | HALF_MASK[f2]) & step);
    return mask;
}

inline bool paneConnects(uint8_t nb) {
    const BlockInfo& i = blockInfo(nb);
    return isPane(nb) || (i.solid && i.model == ModelCube && i.mask == 0xFF);
}
// Pane arms: bit 0 +X, 1 -X, 2 +Z, 3 -Z.
template <class Get> uint8_t paneConnections(Get get, int x, int y, int z) {
    return (uint8_t)(paneConnects(get(x + 1, y, z)) | paneConnects(get(x - 1, y, z)) << 1 |
                     paneConnects(get(x, y, z + 1)) << 2 | paneConnects(get(x, y, z - 1)) << 3);
}
// The neighbour-dependent part of a block's geometry: pane arms, or the occupied cells of a cube.
template <class Get> uint8_t blockShape(uint8_t b, Get get, int x, int y, int z) {
    return isPane(b) ? paneConnections(get, x, y, z) : stairMask(b, get, x, y, z);
}
int blockBoxes(uint8_t b, uint8_t shape, Box out[8]);  // selection and collision boxes

extern const uint8_t kShapeMaterials[SHAPE_MATERIALS];  // full blocks that have shaped forms; append only (block ids)
extern const uint8_t kPanelMaterials[PANEL_MATERIALS];  // likewise for panels
const std::vector<uint8_t>& creativeItems();  // inventory layout
extern const char* const kBlockTextures[];
extern const int kBlockTextureCount;

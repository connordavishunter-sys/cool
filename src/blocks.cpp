#include "blocks.hpp"

#include <array>
#include <string>

const char* const kBlockTextures[] = {
    "grass_top", "grass_side", "dirt", "stone", "sand", "planks", "log_side", "log_bottom",
    "glass", "water", "white_light", "red_light", "orange_light", "yellow_light", "blue_light", "purple_light", "leaves", "lava",
    "red_stained_glass", "orange_stained_glass", "yellow_stained_glass", "green_stained_glass", "purple_stained_glass",
    "red_flower", "orange_flower", "yellow_flower", "green_flower", "blue_flower", "purple_flower",
    "cloud",
};
const int kBlockTextureCount = sizeof(kBlockTextures) / sizeof(kBlockTextures[0]);

const uint8_t kShapeMaterials[SHAPE_MATERIALS] = {Planks, Log, WhiteLight, RedLight, OrangeLight, YellowLight, BlueLight, PurpleLight, Stone};
const uint8_t kPanelMaterials[PANEL_MATERIALS] = {Stone, Planks, Log};
static const char* const kShapeNames[] = {"Slab", "Vertical Slab", "Stairs", "Sideways Stairs"};
static const int kCanonicalState[] = {0, 5, 9, 17};  // the orientation shown in the inventory

#define ALL(t) {t, t, t, t, t, t}
static const BlockInfo kBase[ShapedBase] = {
    {"Air",                  ALL(0),             false, false, KindOpaque,      {0, 0, 0},    0},
    {"Grass",                {1, 1, 0, 2, 1, 1}, true,  true,  KindOpaque,      {0, 0, 0},    0.6f},
    {"Dirt",                 ALL(2),             true,  true,  KindOpaque,      {0, 0, 0},    0.5f},
    {"Stone",                ALL(3),             true,  true,  KindOpaque,      {0, 0, 0},    2.0f},
    {"Sand",                 ALL(4),             true,  true,  KindOpaque,      {0, 0, 0},    0.5f},
    {"Planks",               ALL(5),             true,  true,  KindOpaque,      {0, 0, 0},    1.5f},
    {"Log",                  {6, 6, 7, 7, 6, 6}, true,  true,  KindOpaque,      {0, 0, 0},    1.5f},
    {"Glass",                ALL(8),             false, true,  KindCutout,      {0, 0, 0},    0.4f},
    {"Water",                ALL(9),             false, false, KindWater,       {0, 0, 0},    -1},
    {"White Light",          ALL(10),            true,  true,  KindOpaque,      {15, 15, 15}, 0.4f},
    {"Red Light",            ALL(11),            true,  true,  KindOpaque,      {15, 2, 2},   0.4f},
    {"Orange Light",         ALL(12),            true,  true,  KindOpaque,      {15, 8, 1},   0.4f},
    {"Yellow Light",         ALL(13),            true,  true,  KindOpaque,      {15, 14, 3},  0.4f},
    {"Blue Light",           ALL(14),            true,  true,  KindOpaque,      {2, 6, 15},   0.4f},
    {"Purple Light",         ALL(15),            true,  true,  KindOpaque,      {10, 3, 15},  0.4f},
    {"Leaves",               ALL(16),            false, true,  KindCutout,      {0, 0, 0},    0.25f},
    {"Lava",                 ALL(17),            false, false, KindOpaque,      {15, 9, 3},   -1},
    {"Red Stained Glass",    ALL(18),            false, true,  KindTranslucent, {0, 0, 0},    0.4f},
    {"Orange Stained Glass", ALL(19),            false, true,  KindTranslucent, {0, 0, 0},    0.4f},
    {"Yellow Stained Glass", ALL(20),            false, true,  KindTranslucent, {0, 0, 0},    0.4f},
    {"Green Stained Glass",  ALL(21),            false, true,  KindTranslucent, {0, 0, 0},    0.4f},
    {"Purple Stained Glass", ALL(22),            false, true,  KindTranslucent, {0, 0, 0},    0.4f},
    {"Red Flower",           ALL(23),            false, false, KindCutout,      {0, 0, 0},    0,    ModelCross},
    {"Orange Flower",        ALL(24),            false, false, KindCutout,      {0, 0, 0},    0,    ModelCross},
    {"Yellow Flower",        ALL(25),            false, false, KindCutout,      {0, 0, 0},    0,    ModelCross},
    {"Green Flower",         ALL(26),            false, false, KindCutout,      {0, 0, 0},    0,    ModelCross},
    {"Blue Flower",          ALL(27),            false, false, KindCutout,      {0, 0, 0},    0,    ModelCross},
    {"Purple Flower",        ALL(28),            false, false, KindCutout,      {0, 0, 0},    0,    ModelCross},
    {"Glass Pane",           ALL(8),             false, true,  KindCutout,      {0, 0, 0},    0.3f, ModelPane},
    {"Red Stained Pane",     ALL(18),            false, true,  KindTranslucent, {0, 0, 0},    0.3f, ModelPane},
    {"Orange Stained Pane",  ALL(19),            false, true,  KindTranslucent, {0, 0, 0},    0.3f, ModelPane},
    {"Yellow Stained Pane",  ALL(20),            false, true,  KindTranslucent, {0, 0, 0},    0.3f, ModelPane},
    {"Green Stained Pane",   ALL(21),            false, true,  KindTranslucent, {0, 0, 0},    0.3f, ModelPane},
    {"Purple Stained Pane",  ALL(22),            false, true,  KindTranslucent, {0, 0, 0},    0.3f, ModelPane},
};
#undef ALL

static uint8_t stateMask(int s) {
    if (s < 2) return s ? 0xF0 : 0x0F;
    if (s < 6) return HALF_MASK[s - 2];
    if (s < 14) {
        uint8_t h = HALF_MASK[(s - 6) & 3];
        return s >= 10 ? 0xF0 | (h & 0x0F) : 0x0F | (h & 0xF0);
    }
    int q = s - 14;
    return (uint8_t)~(1 << q | 1 << (q + 4));
}

static std::array<BlockInfo, BlockCount> buildBlocks() {
    static std::string names[SHAPE_MATERIALS * 4];
    std::array<BlockInfo, BlockCount> t{};
    for (int i = 0; i < ShapedBase; ++i) t[i] = kBase[i];
    for (int m = 0; m < SHAPE_MATERIALS; ++m)
        for (int s = 0; s < SHAPE_STATES; ++s) {
            int shape = shapeOf(s);
            std::string& name = names[m * 4 + shape];
            name = std::string(kBase[kShapeMaterials[m]].name) + " " + kShapeNames[shape];
            BlockInfo& b = t[ShapedBase + m * SHAPE_STATES + s];
            b = kBase[kShapeMaterials[m]];
            b.name = name.c_str();
            b.opaque = false;
            b.mask = stateMask(s);
        }
    static std::string panelNames[PANEL_MATERIALS];
    for (int m = 0; m < PANEL_MATERIALS; ++m)
        for (int s = 0; s < PANEL_STATES; ++s) {
            panelNames[m] = std::string(kBase[kPanelMaterials[m]].name) + " Panel";
            BlockInfo& b = t[PanelBase + m * PANEL_STATES + s];
            b = kBase[kPanelMaterials[m]];
            b.name = panelNames[m].c_str();
            b.opaque = false;
            b.mask = s % 7 >= 3 ? (s >= 7 ? 0xF0 : 0x0F) : 0;  // half the block or more covers a layer of cells
        }
    return t;
}
static const std::array<BlockInfo, BlockCount> kBlocks = buildBlocks();

const BlockInfo& blockInfo(uint8_t b) { return kBlocks[b < BlockCount ? b : 0]; }

Shape shapeOf(int s) { return s < 2 ? ShapeSlab : s < 6 ? ShapeVSlab : s < 14 ? ShapeStairs : ShapeSideStairs; }
uint8_t shapedId(uint8_t material, int state) {
    for (int m = 0; m < SHAPE_MATERIALS; ++m)
        if (kShapeMaterials[m] == material) return (uint8_t)(ShapedBase + m * SHAPE_STATES + state);
    return Air;
}
uint8_t shapeMaterial(uint8_t b) { return kShapeMaterials[(b - ShapedBase) / SHAPE_STATES]; }
uint8_t panelId(uint8_t material, int eighths, bool top) {
    for (int m = 0; m < PANEL_MATERIALS; ++m)
        if (kPanelMaterials[m] == material) return eighths >= 8 ? material : (uint8_t)(PanelBase + m * PANEL_STATES + top * 7 + eighths - 1);
    return Air;
}
uint8_t panelMaterial(uint8_t b) { return kPanelMaterials[(b - PanelBase) / PANEL_STATES]; }

const uint8_t* tintLoss(uint8_t b) {
    // Matched to the textures' average colour; indexed by the stained glass texture layer (18-22).
    static const uint8_t loss[6][3] = {{0, 0, 0}, {1, 12, 12}, {0, 7, 13}, {0, 2, 10}, {9, 1, 10}, {4, 11, 3}};
    int t = blockInfo(b).tex[0] - 17;
    return loss[blockInfo(b).kind == KindTranslucent && t >= 1 && t <= 5 ? t : 0];
}
int shapeState(uint8_t b) { return (b - ShapedBase) % SHAPE_STATES; }
uint8_t itemOf(uint8_t b) {
    if (isPanel(b)) return panelId(panelMaterial(b), 1, false);
    return isShaped(b) ? shapedId(shapeMaterial(b), kCanonicalState[shapeOf(shapeState(b))]) : b;
}

int blockBoxes(uint8_t b, uint8_t shape, Box out[8]) {
    const BlockInfo& info = blockInfo(b);
    int n = 0;
    auto add = [&](float x0, float y0, float z0, float x1, float y1, float z1, uint8_t skip = 0) {
        out[n++] = {{x0, y0, z0}, {x1, y1, z1}, skip};
    };
    if (isPanel(b)) {
        float h = panelEighths(b) / 8.0f;
        panelTop(b) ? add(0, 1 - h, 0, 1, 1, 1) : add(0, 0, 0, 1, h, 1);
    } else if (info.model == ModelCross) {
        add(0.25f, 0, 0.25f, 0.75f, 0.85f, 0.75f);
    } else if (info.model == ModelPane) {
        // A bar along X through the middle, plus separate arms along Z; a lone pane is a cross.
        const float a = 7 / 16.0f, c = 9 / 16.0f;
        uint8_t conn = shape ? shape : 15;
        bool x = conn & 3, px = conn & 1, nx = conn & 2, pz = conn & 4, nz = conn & 8;
        if (x || !(pz && nz)) {
            if (x) add(nx ? 0 : a, 0, a, px ? 1 : c, 1, c);
            if (pz) add(a, 0, x ? c : a, c, 1, 1, x ? 1 << 5 : 0);
            if (nz) add(a, 0, 0, c, 1, x ? a : c, x ? 1 << 4 : 0);
        } else {
            add(a, 0, 0, c, 1, 1);
        }
    } else if (shape == 0xFF) {
        add(0, 0, 0, 1, 1, 1);
    } else {
        for (int i = 0; i < 8; ++i)
            if (shape >> i & 1) {
                float x = (i & 1) * 0.5f, z = (i >> 1 & 1) * 0.5f, y = (i >> 2) * 0.5f;
                add(x, y, z, x + 0.5f, y + 0.5f, z + 0.5f);
            }
    }
    return n;
}

const std::vector<uint8_t>& creativeItems() {
    static const std::vector<uint8_t> items = [] {
        std::vector<uint8_t> v = {
            Grass, Dirt, Stone, Sand, Planks, Log, Leaves, Glass, Water, Lava,
            RedFlower, OrangeFlower, YellowFlower, GreenFlower, BlueFlower, PurpleFlower,
            RedStainedGlass, OrangeStainedGlass, YellowStainedGlass, GreenStainedGlass, PurpleStainedGlass,
            GlassPane, RedStainedPane, OrangeStainedPane, YellowStainedPane, GreenStainedPane, PurpleStainedPane,
            WhiteLight, RedLight, OrangeLight, YellowLight, BlueLight, PurpleLight,
        };
        for (bool lights : {false, true}) {  // building materials and panels first, then the lights
            for (uint8_t m : kShapeMaterials)
                if (isEmissive(m) == lights)
                    for (int s : kCanonicalState) v.push_back(shapedId(m, s));
            if (!lights)
                for (uint8_t m : kPanelMaterials) v.push_back(panelId(m, 1, false));
        }
        return v;
    }();
    return items;
}

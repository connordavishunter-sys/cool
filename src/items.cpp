#include "items.hpp"

#include <algorithm>

const char* itemName(uint8_t id) { return id == ItemFood ? "Food" : blockInfo(id).name; }

int Inventory::count(uint8_t id) const {
    int n = 0;
    for (const auto& s : slots)
        if (s.id == id) n += s.count;
    return n;
}

bool Inventory::fits(uint8_t id, int n) const {
    for (const auto& s : slots) n -= s.empty() ? STACK_MAX : s.id == id ? STACK_MAX - s.count : 0;
    return n <= 0;
}

int Inventory::add(uint8_t id, int n) {
    // Top up existing stacks first, then fill empty slots in order (hotbar first).
    for (int pass = 0; pass < 2 && n > 0; ++pass)
        for (auto& s : slots) {
            if (pass == 0 ? s.empty() || s.id != id : !s.empty()) continue;
            int k = std::min(n, STACK_MAX - s.count);
            s.id = id;
            s.count = (uint8_t)(s.count + k);
            if ((n -= k) == 0) break;
        }
    return n;
}

void Inventory::remove(uint8_t id, int n) {
    for (int i = INV_SIZE - 1; i >= 0 && n > 0; --i) {  // take from storage before the hotbar
        auto& s = slots[i];
        if (s.id != id) continue;
        int k = std::min(n, (int)s.count);
        s.count = (uint8_t)(s.count - k);
        n -= k;
        if (s.empty()) s = {};
    }
}

// Blocks that don't generate naturally are crafted from ones that do. Stained glass takes its
// colour from a natural block of a similar hue.
static const Recipe kBaseRecipes[] = {
    {{Planks, 4}, {{Log, 1}}},
    {{Glass, 1}, {{Sand, 1}}},
    {{RedStainedGlass, 1}, {{Glass, 1}, {Dirt, 1}}},
    {{OrangeStainedGlass, 1}, {{Glass, 1}, {Log, 1}}},
    {{YellowStainedGlass, 1}, {{Glass, 1}, {Sand, 1}}},
    {{GreenStainedGlass, 1}, {{Glass, 1}, {Leaves, 1}}},
    {{PurpleStainedGlass, 1}, {{Glass, 1}, {Stone, 1}}},
    {{WhiteLight, 1}, {{Glass, 2}, {Planks, 1}}},
    {{RedLight, 1}, {{WhiteLight, 1}, {RedStainedGlass, 1}}},
    {{OrangeLight, 1}, {{WhiteLight, 1}, {OrangeStainedGlass, 1}}},
    {{YellowLight, 1}, {{WhiteLight, 1}, {YellowStainedGlass, 1}}},
    {{PurpleLight, 1}, {{WhiteLight, 1}, {PurpleStainedGlass, 1}}},
    {{BlueLight, 1}, {{WhiteLight, 1}, {Stone, 1}}},
};

const std::vector<Recipe>& recipes() {
    static const std::vector<Recipe> list = [] {
        std::vector<Recipe> v(std::begin(kBaseRecipes), std::end(kBaseRecipes));
        v.push_back({{GlassPane, 16}, {{Glass, 6}}});
        for (int i = 0; i < 5; ++i) v.push_back({{(uint8_t)(RedStainedPane + i), 16}, {{(uint8_t)(RedStainedGlass + i), 6}}});
        for (uint8_t m : kShapeMaterials) {
            uint8_t slab = itemOf(shapedId(m, 0)), stairs = itemOf(shapedId(m, 6));
            v.push_back({{slab, 6}, {{m, 3}}});
            v.push_back({{itemOf(shapedId(m, 2)), 1}, {{slab, 1}}});
            v.push_back({{stairs, 4}, {{m, 6}}});
            v.push_back({{itemOf(shapedId(m, 14)), 1}, {{stairs, 1}}});
        }
        return v;
    }();
    return list;
}

#pragma once
#include "blocks.hpp"

#include <vector>

// Item ids share the block id space; items that aren't blocks count down from the top.
enum Item : BlockId { ItemFood = 0xFFFF };
constexpr int LEGACY_FOOD = 128, OLD_FOOD = 255;  // food's id in saves from before shaped blocks, and before 16-bit ids

constexpr int STACK_MAX = 64, INV_SIZE = 36;  // inventory slots 0..8 are the hotbar

struct ItemStack {
    BlockId id = 0;
    uint8_t count = 0;
    bool empty() const { return count == 0; }
};

inline bool isBlockItem(BlockId id) { return id > Air && id < BlockCount; }
const char* itemName(BlockId id);

struct Inventory {
    ItemStack slots[INV_SIZE];

    int count(BlockId id) const;
    bool fits(BlockId id, int n) const;
    int add(BlockId id, int n);  // returns how many didn't fit
    void remove(BlockId id, int n);
};

struct Recipe {
    ItemStack out, in[2];
};
const std::vector<Recipe>& recipes();

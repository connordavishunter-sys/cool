#pragma once
#include "blocks.hpp"

#include <vector>

// Item ids share the block id space; items that aren't blocks count down from 255.
enum Item : uint8_t { ItemFood = 255 };
constexpr uint8_t LEGACY_FOOD = 128;  // food's id in saves from before shaped blocks

constexpr int STACK_MAX = 64, INV_SIZE = 36;  // inventory slots 0..8 are the hotbar

struct ItemStack {
    uint8_t id = 0, count = 0;
    bool empty() const { return count == 0; }
};

inline bool isBlockItem(uint8_t id) { return id > Air && id < BlockCount; }
const char* itemName(uint8_t id);

struct Inventory {
    ItemStack slots[INV_SIZE];

    int count(uint8_t id) const;
    bool fits(uint8_t id, int n) const;
    int add(uint8_t id, int n);  // returns how many didn't fit
    void remove(uint8_t id, int n);
};

struct Recipe {
    ItemStack out, in[2];
};
const std::vector<Recipe>& recipes();

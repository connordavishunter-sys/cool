#pragma once
#include "world.hpp"

#include <glm/glm.hpp>

struct Player {
    static constexpr double HALF_WIDTH = 0.3, HEIGHT = 1.8, EYE = 1.62, STEP = 0.6;

    glm::dvec3 pos{8.5, 100, 8.5};  // feet
    glm::dvec3 vel{0};
    float yaw = 0, pitch = 0;
    bool flying = false, onGround = false, inWater = false;  // inWater: swimming in any liquid
    bool inLava = false, sprinting = false, jumped = false;
    bool canSprint = true;

    glm::dvec3 eye() const { return pos + glm::dvec3(0, EYE, 0); }
    glm::vec3 look() const;
    void update(const World& world, const uint8_t* keys, double dt);
    bool intersects(const World& world, glm::ivec3 p, uint8_t b) const;  // would block b at p overlap the player

private:
    bool hitWall = false;  // ran into something horizontally this update
    double clip(const World& world, int axis, double delta) const;
    void moveAxis(const World& world, int axis, double delta);
};

struct RayHit {
    glm::ivec3 block, normal;
    glm::dvec3 point;
};
bool raycast(const World& world, glm::dvec3 origin, glm::dvec3 dir, double maxDist, RayHit& hit);
void selectionBox(const World& world, glm::ivec3 p, glm::dvec3& lo, glm::dvec3& hi);  // bounds of the block's shape

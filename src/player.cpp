#include "player.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>

glm::vec3 Player::look() const {
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

struct WorldBox { glm::dvec3 lo, hi; };

// World-space boxes of block b at p, kept in doubles so they stay exact far from the origin.
static int worldBoxes(const World& world, glm::ivec3 p, uint8_t b, WorldBox out[8]) {
    Box boxes[8];
    int n = blockBoxes(b, blockShape(b, [&](int x, int y, int z) { return world.get(x, y, z); }, p.x, p.y, p.z), boxes);
    for (int i = 0; i < n; ++i)
        out[i] = {glm::dvec3(p) + glm::dvec3(boxes[i].lo[0], boxes[i].lo[1], boxes[i].lo[2]),
                  glm::dvec3(p) + glm::dvec3(boxes[i].hi[0], boxes[i].hi[1], boxes[i].hi[2])};
    return n;
}

// How far the player can move along an axis, up to delta, before touching a solid box.
double Player::clip(const World& world, int axis, double delta) const {
    glm::dvec3 lo = pos - glm::dvec3(HALF_WIDTH, 0, HALF_WIDTH), hi = pos + glm::dvec3(HALF_WIDTH, HEIGHT, HALF_WIDTH);
    glm::dvec3 qlo = lo, qhi = hi;
    (delta < 0 ? qlo : qhi)[axis] += delta;
    const double e = 1e-7;
    double d = delta;
    for (int y = (int)std::floor(qlo.y); y <= (int)std::floor(qhi.y); ++y)
        for (int z = (int)std::floor(qlo.z); z <= (int)std::floor(qhi.z); ++z)
            for (int x = (int)std::floor(qlo.x); x <= (int)std::floor(qhi.x); ++x) {
                uint8_t b = world.get(x, y, z);
                if (!blockInfo(b).solid) continue;
                WorldBox boxes[8];
                int n = worldBoxes(world, {x, y, z}, b, boxes);
                for (int i = 0; i < n; ++i) {
                    const WorldBox& bx = boxes[i];
                    bool overlaps = true;
                    for (int a = 0; a < 3; ++a)
                        if (a != axis) overlaps &= bx.lo[a] < hi[a] - e && bx.hi[a] > lo[a] + e;
                    if (!overlaps) continue;
                    if (delta > 0 && bx.lo[axis] >= hi[axis] - e) d = std::min(d, bx.lo[axis] - hi[axis]);
                    if (delta < 0 && bx.hi[axis] <= lo[axis] + e) d = std::max(d, bx.hi[axis] - lo[axis]);
                }
            }
    return delta > 0 ? std::max(d, 0.0) : std::min(d, 0.0);
}

bool Player::intersects(const World& world, glm::ivec3 p, uint8_t b) const {
    WorldBox boxes[8];
    int n = worldBoxes(world, p, b, boxes);
    glm::dvec3 lo = pos - glm::dvec3(HALF_WIDTH, 0, HALF_WIDTH), hi = pos + glm::dvec3(HALF_WIDTH, HEIGHT, HALF_WIDTH);
    for (int i = 0; i < n; ++i)
        if (glm::all(glm::greaterThan(boxes[i].hi, lo)) && glm::all(glm::lessThan(boxes[i].lo, hi))) return true;
    return false;
}

void Player::moveAxis(const World& world, int axis, double delta) {
    double d = clip(world, axis, delta);
    if (d != delta && axis != 1 && onGround && !flying) {
        // Walk up slabs and stairs: lift, move, then settle back down.
        glm::dvec3 start = pos;
        pos.y += clip(world, 1, STEP);
        double lifted = clip(world, axis, delta);
        if (std::abs(lifted) > std::abs(d) + 1e-6) {
            pos[axis] += lifted;
            pos.y += clip(world, 1, start.y - pos.y);
            return;
        }
        pos = start;
    }
    pos[axis] += d;
    if (d == delta) return;
    if (axis == 1 && delta < 0) onGround = true;
    if (axis != 1) hitWall = true;
    vel[axis] = 0;
}

void Player::update(const World& world, const uint8_t* keys, double dt) {
    glm::dvec3 fwd(std::sin(yaw), 0, -std::cos(yaw)), right(std::cos(yaw), 0, std::sin(yaw));
    glm::dvec3 wish = fwd * double(keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]) + right * double(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]);
    if (glm::length(wish) > 0) wish = glm::normalize(wish);
    bool sprint = keys[SDL_SCANCODE_LCTRL] && canSprint;
    bool up = keys[SDL_SCANCODE_SPACE], down = keys[SDL_SCANCODE_LSHIFT];
    int bx = (int)std::floor(pos.x), bz = (int)std::floor(pos.z);
    inWater = isLiquid(world.get(bx, (int)std::floor(pos.y + 0.4), bz));
    inLava = world.get(bx, (int)std::floor(pos.y + 0.1), bz) == Lava || world.get(bx, (int)std::floor(pos.y + 1.2), bz) == Lava;
    sprinting = sprint && glm::length(wish) > 0;
    jumped = false;

    if (flying) {
        double speed = sprint ? 22.0 : 11.0;
        glm::dvec3 target = wish * speed + glm::dvec3(0, (up - down) * speed * 0.7, 0);
        vel += (target - vel) * std::min(1.0, dt * 10);
    } else {
        double speed = (sprint ? 5.6 : 4.3) * (inLava ? 0.35 : inWater ? 0.6 : 1.0);
        double accel = onGround || inWater ? 14.0 : 3.0;
        glm::dvec3 target = wish * speed;
        vel.x += (target.x - vel.x) * std::min(1.0, dt * accel);
        vel.z += (target.z - vel.z) * std::min(1.0, dt * accel);
        if (inWater) {
            vel.y += (up ? 4.0 - vel.y : -2.0 - vel.y) * std::min(1.0, dt * 4);
        } else {
            vel.y = std::max(vel.y - 32.0 * dt, -78.0);
            if (up && onGround) { vel.y = 9.0; jumped = true; }
        }
    }

    onGround = hitWall = false;
    glm::dvec3 step = vel * dt;
    int n = (int)std::ceil(glm::length(step) / 0.4) + 1;
    for (int i = 0; i < n; ++i) {
        moveAxis(world, 1, step.y / n);
        moveAxis(world, 0, step.x / n);
        moveAxis(world, 2, step.z / n);
    }
    if (onGround && flying) flying = false;
    // Swimming into a ledge hops up onto it; the surface floats about a block below the shore otherwise.
    bool feetInLiquid = isLiquid(world.get((int)std::floor(pos.x), (int)std::floor(pos.y + 0.1), (int)std::floor(pos.z)));
    if (feetInLiquid && up && hitWall && !flying) vel.y = std::max(vel.y, 9.5);
    if (pos.y < -64) { pos.y = CH + 10; vel = glm::dvec3(0); }
}

// Entry distance of the ray into a box, or -1; normal is the face it enters through.
static double rayBox(glm::dvec3 o, glm::dvec3 d, const WorldBox& b, glm::ivec3& normal) {
    double t0 = 0, t1 = 1e30;
    int axis = -1;
    for (int a = 0; a < 3; ++a) {
        if (d[a] == 0) {
            if (o[a] < b.lo[a] || o[a] > b.hi[a]) return -1;
            continue;
        }
        double ta = (b.lo[a] - o[a]) / d[a], tb = (b.hi[a] - o[a]) / d[a];
        if (ta > tb) std::swap(ta, tb);
        if (ta > t0) { t0 = ta; axis = a; }
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1;
    }
    if (axis < 0) return -1;  // starts inside
    normal = glm::ivec3(0);
    normal[axis] = d[axis] > 0 ? -1 : 1;
    return t0;
}

bool raycast(const World& world, glm::dvec3 o, glm::dvec3 d, double maxDist, RayHit& hit) {
    glm::ivec3 cell(std::floor(o.x), std::floor(o.y), std::floor(o.z));
    glm::ivec3 step(d.x > 0 ? 1 : -1, d.y > 0 ? 1 : -1, d.z > 0 ? 1 : -1);
    glm::dvec3 tDelta, tMax;
    for (int i = 0; i < 3; ++i) {
        tDelta[i] = d[i] != 0 ? std::abs(1.0 / d[i]) : 1e30;
        double edge = step[i] > 0 ? cell[i] + 1 - o[i] : o[i] - cell[i];
        tMax[i] = d[i] != 0 ? edge * tDelta[i] : 1e30;
    }
    double t = 0;
    while (t <= maxDist) {
        uint8_t b = world.get(cell.x, cell.y, cell.z);
        if (b != Air && !isLiquid(b)) {
            WorldBox boxes[8];
            int n = worldBoxes(world, cell, b, boxes);
            double best = 1e30;
            for (int i = 0; i < n; ++i) {
                glm::ivec3 normal;
                double tb = rayBox(o, d, boxes[i], normal);
                if (tb >= 0 && tb < best && tb <= maxDist) { best = tb; hit.normal = normal; }
            }
            if (best < 1e30) {
                hit.block = cell;
                hit.point = o + d * best;
                return true;
            }
        }
        int a = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        t = tMax[a];
        tMax[a] += tDelta[a];
        cell[a] += step[a];
    }
    return false;
}

void selectionBox(const World& world, glm::ivec3 p, glm::dvec3& lo, glm::dvec3& hi) {
    WorldBox boxes[8];
    int n = worldBoxes(world, p, world.get(p.x, p.y, p.z), boxes);
    lo = glm::dvec3(p) + 1.0;
    hi = glm::dvec3(p);
    for (int i = 0; i < n; ++i) {
        lo = glm::min(lo, boxes[i].lo);
        hi = glm::max(hi, boxes[i].hi);
    }
}
